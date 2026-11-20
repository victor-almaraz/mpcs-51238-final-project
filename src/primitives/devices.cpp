// Sources and sinks: audio, MIDI, OSC, serial, and web (primitives.md
// §7.2, §7.3; io.md §4, §5). Data other than audio crosses to the I/O
// layer through ports (io.h).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include <espresso/primitives/event.h>
#include <espresso/primitives/io.h>
#include <espresso/primitives/pipe.h>
#include <espresso/primitives/primitive.h>
#include <espresso/primitives/realtime.h>

#include "limiter.h"
#include "node.h"

namespace primitives {

namespace {

// The sample offset for a message received at @p time. Messages that
// arrived during the previous block period land at their relative
// position in this block, which trades one block of latency for
// jitter-free timing (§7.2.2).
std::uint32_t offsetFor(double time, const blockContext& ctx) noexcept {
    const double period = ctx.numSamples / ctx.sampleRate;
    const double offset = std::floor((time - (ctx.hostTime - period)) * ctx.sampleRate);
    return static_cast<std::uint32_t>(std::clamp(offset, 0.0, ctx.numSamples - 1.0));
}

// The device clock time of sample @p offset in this block.
double timeFor(std::uint32_t offset, const blockContext& ctx) noexcept {
    return ctx.hostTime + offset / ctx.sampleRate;
}

std::uint16_t udpPort(nodeContext& c) { return static_cast<std::uint16_t>(c.number("port")); }

// The bytes a packet holds, never more than its buffer, whatever its size
// field claims.
template <typename Packet>
auto packetBytes(const Packet& p) noexcept {
    return std::span(p.bytes).first(std::min<std::size_t>(p.size, p.bytes.size()));
}

// Pops at most one queue's capacity of packets from @p port into
// @p handle, so a producer that keeps up with the audio thread cannot
// keep it in the loop.
template <typename Port, typename Packet, typename Handle>
void drain(const std::shared_ptr<Port>& port, Packet& packet, Handle&& handle) noexcept {
    if (!port) return;
    for (std::size_t k = 0; k < port->queue.capacity() && port->queue.pop(packet); ++k) handle(packet);
}

// An input port that survives adoption: the adopting source reads the
// outgoing source's port, and every earlier port that a late delivery may
// still push into, as well as its own, so messages queued in them at a
// swap, or delivered while the engine switches ports, still arrive (§9.2).
template <typename Port>
class adoptedPorts {
public:
    // @p io counts messages lost when a port cannot be carried.
    adoptedPorts(std::shared_ptr<Port> port, ioHub& io) : current_(std::move(port)), io_(io) {}

    const std::shared_ptr<Port>& current() const noexcept { return current_; }

    // Takes over @p outgoing's port and its earlier ports that are still
    // open (audio thread; swaps, not copies, so no reference count changes,
    // and a finished port stays with @p outgoing, which is freed on the
    // control thread). @p outgoing never runs again. The own port activates
    // now, before the engine deactivates the outgoing one, so some port of
    // this node is active at every moment; the hub delivers each message to
    // only one of them.
    void adopt(adoptedPorts& outgoing) noexcept {
        for (auto& p : outgoing.earlier_) carry(p);  // older first
        carry(outgoing.current_);
        current_->setActive(true);
    }

    // Drains the earlier ports, oldest first, then the own one.
    template <typename Packet, typename Handle>
    void drainAll(Packet& packet, Handle&& handle) noexcept {
        for (auto& p : earlier_) drain(p, packet, handle);
        drain(current_, packet, handle);
    }

private:
    // Earlier ports one node reads at once. A port leaves the set once it
    // is closed and drained, within a block of its deactivation unless a
    // delivery stalls mid-push, so a few slots cover any run of swaps.
    static constexpr std::size_t slots = 8;

    // True when nothing is left in @p port and nothing can still arrive:
    // closed first, so a push that finished before it is seen as not empty.
    static bool finished(const std::shared_ptr<Port>& port) noexcept {
        return !port || (port->closed() && port->queue.empty());
    }

    // Moves @p port into a free slot unless it is finished. With every
    // slot taken, the port stays behind and counts as a drop: what it
    // holds, or still receives, is lost.
    void carry(std::shared_ptr<Port>& port) noexcept {
        if (finished(port)) return;
        for (auto& slot : earlier_) {
            if (finished(slot)) {
                std::swap(slot, port);  // a finished port goes back to the outgoing node
                return;
            }
        }
        io_.countDrop();
    }

    std::shared_ptr<Port> current_;
    std::array<std::shared_ptr<Port>, slots> earlier_;  // oldest first, with gaps
    ioHub& io_;
};

// --- Audio ----------------------------------------------------------------

class audioIn final : public nodeBase {
public:
    explicit audioIn(nodeContext& c)
        : nodeBase(c), first_(static_cast<std::size_t>(c.number("channel"))), out_(c.audioOut("out")) {}

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const auto& inputs = ctx.device.inputs;
        const auto n = static_cast<std::size_t>(ctx.numSamples);
        const auto at = static_cast<std::size_t>(ctx.deviceOffset);
        for (int ch = 0; ch < out_.channels; ++ch) {
            const std::size_t source = first_ + static_cast<std::size_t>(ch);
            if (source >= inputs.size() || inputs[source].size() < at + n) break;
            emit(out_, ch, inputs[source].subspan(at, n));
        }
    }

private:
    // The first device channel; metadata bounds it to [0, 255].
    std::size_t first_ = 0;
    audioOutput out_;
};

// The last stage before the device. A non-finite sample mutes the sink
// until its plan is replaced and reports a fault; the limiter keeps every
// sample within the ceiling, so a runaway graph cannot send more than
// full scale.
class audioOut final : public nodeBase {
public:
    explicit audioOut(nodeContext& c)
        : nodeBase(c),
          first_(static_cast<std::size_t>(c.number("channel"))),
          ceiling_(static_cast<float>(c.number("ceiling"))),
          in_(c.audioIn("in")),
          io_(c.io()),
          limiter_(c.sampleRate()),
          buffers_(filledArray<blockBuffer, maxChannels>(blockBuffer(c.blockSize()))) {}

    // The limiter's gain carries over; a mute does not: a new plan gets a
    // new chance.
    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        adoptFinite(limiter_, static_cast<audioOut&>(outgoing).limiter_);
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const auto& outputs = ctx.device.outputs;
        if (muted_ || outputs.empty()) return;
        const int channels = in_->channels();
        peakLimiter::inputs in;
        for (int ch = 0; ch < channels; ++ch) {
            in[static_cast<std::size_t>(ch)] = in_->read(ch);
            for (float x : in[static_cast<std::size_t>(ch)]) {
                if (!std::isfinite(x)) {
                    muted_ = true;
                    io_.reportFault(id(), "non-finite audio samples; this output is muted until the next swap");
                    return;
                }
            }
        }
        const auto n = static_cast<std::size_t>(ctx.numSamples);
        limiter_.process(in, channels, buffers_, n, ceiling_);
        const auto at = static_cast<std::size_t>(ctx.deviceOffset);
        for (int ch = 0; ch < channels; ++ch) {
            const std::size_t target = first_ + static_cast<std::size_t>(ch);
            if (target >= outputs.size() || outputs[target].size() < at + n) break;
            emitDevice(outputs[target].subspan(at, n), buffers_[static_cast<std::size_t>(ch)].first(n));
        }
    }

private:
    // The first device channel; metadata bounds it to [0, 255].
    std::size_t first_ = 0;
    float ceiling_ = 1.0f;
    std::shared_ptr<const aPipe> in_;
    ioHub& io_;
    peakLimiter limiter_;
    bool muted_ = false;
    std::array<blockBuffer, maxChannels> buffers_;
};

// --- MIDI -----------------------------------------------------------------

class midiIn final : public nodeBase {
public:
    explicit midiIn(nodeContext& c)
        : nodeBase(c),
          ports_(c.io().openMidiIn(c.id(), c.text("device")), c.io()),
          channel_(static_cast<int>(c.number("channel"))),
          out_(c.eventOut("out")) {
        c.keep(ports_.current());
    }

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        ports_.adopt(static_cast<midiIn&>(outgoing).ports_);
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        midiPacket packet;
        ports_.drainAll(packet, [&](const midiPacket& p) {
            const auto e = fromMidi(packetBytes(p), offsetFor(p.time, ctx));
            if (e && (channel_ == 0 || primitives::channel(*e) + 1 == channel_)) emit(*out_, *e);
        });
    }

private:
    adoptedPorts<midiPort> ports_;
    int channel_ = 0;
    std::shared_ptr<ePipe> out_;
};

class midiOut final : public nodeBase {
public:
    explicit midiOut(nodeContext& c)
        : nodeBase(c),
          port_(c.io().openMidiOut(c.text("device"))),
          channel_(static_cast<int>(c.number("channel"))),
          in_(c.eventIn("in")),
          io_(c.io()) {
        c.keep(port_);
    }

    // A retiring sink sends only note-offs, so a fade neither doubles notes
    // nor leaves them hanging.
    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        for (const event& e : in_->events()) {
            if (!passes(e)) continue;
            event out = e;
            if (channel_ != 0 && isMidi(e.type)) out.payload[0] = static_cast<std::byte>(channel_ - 1);
            midiPacket p;
            p.size = static_cast<std::uint8_t>(toMidi(out, p.bytes));
            if (p.size == 0) continue;
            p.time = timeFor(e.sampleOffset, ctx);
            if (!port_->queue.push(p)) io_.countDrop();  // §7.3.3
        }
    }

private:
    std::shared_ptr<midiPort> port_;
    int channel_ = 0;
    std::shared_ptr<const ePipe> in_;
    ioHub& io_;
};

// --- OSC ------------------------------------------------------------------

class oscIn final : public nodeBase {
public:
    explicit oscIn(nodeContext& c)
        : nodeBase(c),
          ports_(c.io().openOscIn(c.id(), udpPort(c)), c.io()),
          address_(c.text("address")),
          out_(c.eventOut("out")),
          value_(c.controlOut("value")),
          io_(c.io()) {
        if (!out_ && !value_.pipe) c.fail("oscIn needs an out or value output");
        c.keep(ports_.current());
    }

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        ports_.adopt(static_cast<oscIn&>(outgoing).ports_);
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        oscPacket packet;
        ports_.drainAll(packet, [&](const oscPacket& p) {
            const auto message = packetBytes(p);
            if (!address_.empty() && !oscAddressIs(message, address_)) return;
            if (out_) emitOsc(*out_, offsetFor(p.time, ctx), message);
            if (!value_.pipe) return;
            // A non-finite value never enters the graph.
            if (const auto v = oscFirstNumber(message); v && std::isfinite(*v)) {
                emitAll(value_, *v);
            } else if (v) {
                io_.countDrop();
            }
        });
    }

private:
    adoptedPorts<oscPort> ports_;
    std::string address_;
    std::shared_ptr<ePipe> out_;
    controlOutput value_;
    ioHub& io_;
};

class oscOut final : public nodeBase {
public:
    explicit oscOut(nodeContext& c)
        : nodeBase(c),
          port_(c.io().openOscOut(udpPort(c))),
          address_(c.text("address")),
          in_(c.eventIn("in")),
          value_(c.signalIn("value")),
          io_(c.io()) {
        if (!in_ && value_.isStatic()) c.fail("oscOut needs an in or value input");
        if (!value_.isStatic() && (address_.empty() || address_.front() != '/'))
            c.fail("address must start with /");
        c.keep(port_);
    }

    // The last value sent, only when it was sent to the same place.
    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        const auto& o = static_cast<const oscOut&>(outgoing);
        if (o.address_ == address_ && o.port_->udpPort() == port_->udpPort()) last_ = o.last_;
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        if (retiring()) return;
        if (in_) {
            for (const event& e : in_->events()) {
                if (e.type != eventType::osc) continue;
                const auto data = in_->data(oscData(e));
                oscPacket p;
                if (data.size() > p.bytes.size()) {
                    io_.countDrop();
                    continue;
                }
                std::ranges::copy(data, p.bytes.begin());
                p.size = static_cast<std::uint16_t>(data.size());
                p.time = timeFor(e.sampleOffset, ctx);
                send(p);
            }
        }
        // A control value is sent when it changes.
        if (!value_.isStatic()) {
            const float v = value_.value();
            if (v != last_) {
                last_ = v;
                oscPacket p;
                p.size = static_cast<std::uint16_t>(encodeOscFloat(p.bytes, address_, v));
                p.time = ctx.hostTime;
                if (p.size > 0) send(p);
            }
        }
    }

private:
    void send(const oscPacket& p) noexcept {
        if (!port_->queue.push(p)) io_.countDrop();
    }

    std::shared_ptr<oscPort> port_;
    std::string address_;
    std::shared_ptr<const ePipe> in_;
    signal value_;
    ioHub& io_;
    // The last value sent; NaN, unequal to every value, before the first.
    float last_ = notANumber;
};

// --- Serial (io.md §4) ------------------------------------------------------

std::uint32_t baud(nodeContext& c) { return static_cast<std::uint32_t>(c.number("baud")); }

// The values @p f uses, never more than it holds, whatever its count claims.
std::span<const float> frameValues(const dataFrame& f) noexcept {
    return std::span(f.values).first(std::min<std::size_t>(f.count, f.values.size()));
}

// A source of frames from a port the I/O layer fills (io.md §4.7, §5.1):
// value i to channel i of :value, leaving channels a frame lacks or marks
// missing (§8.2), and a value event per frame on :out carrying value 0,
// when it has one. A frame with a non-finite value never enters the graph.
template <typename Port>
class frameSource : public nodeBase {
public:
    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        ports_.adopt(static_cast<frameSource&>(outgoing).ports_);
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        dataFrame frame;
        ports_.drainAll(frame, [&](const dataFrame& f) {
            const auto values = frameValues(f);
            if (values.empty()) return;
            if (!std::ranges::all_of(values, [](float v) { return std::isfinite(v); })) {
                io_.countDrop();
                return;
            }
            if (value_.pipe) {
                const auto channels = std::min(values.size(), static_cast<std::size_t>(value_.channels));
                for (std::size_t ch = 0; ch < channels; ++ch) {
                    if (f.has(ch)) emit(*value_.pipe, static_cast<int>(ch), values[ch]);
                }
            }
            if (out_ && f.has(0)) emit(*out_, valueEvent(offsetFor(f.time, ctx), values.front()));
        });
    }

protected:
    // Reads @p port for the node of @p c, a @p type.
    frameSource(nodeContext& c, std::shared_ptr<Port> port, std::string_view type)
        : nodeBase(c),
          ports_(std::move(port), c.io()),
          out_(c.eventOut("out")),
          value_(c.controlOut("value")),
          io_(c.io()) {
        if (!out_ && !value_.pipe) c.fail(std::string(type) + " needs an out or value output");
        c.keep(ports_.current());
    }

private:
    adoptedPorts<Port> ports_;
    std::shared_ptr<ePipe> out_;
    controlOutput value_;
    ioHub& io_;
};

class serialIn final : public frameSource<serialPort> {
public:
    explicit serialIn(nodeContext& c)
        : frameSource(c, c.io().openSerialIn(c.id(), c.text("device"), baud(c)), "serialIn") {}
};

// The frame an event sends (io.md §4.8, §7.2): the channels of @p value
// when it is bound, otherwise the event's number; nothing when a value is
// not finite, as it has no line or JSON number to send.
std::optional<dataFrame> frameFor(const event& e, const signal& value, const blockContext& ctx) noexcept {
    dataFrame f;
    f.time = timeFor(e.sampleOffset, ctx);
    if (value.isStatic()) {
        f.values[0] = number(e);
        f.count = 1;
    } else {
        const int channels = std::min(value.channels(), maxChannels);
        for (int ch = 0; ch < channels; ++ch) f.values[static_cast<std::size_t>(ch)] = value.value(ch);
        f.count = static_cast<std::uint8_t>(channels);
    }
    if (!std::ranges::all_of(frameValues(f), [](float v) { return std::isfinite(v); })) return std::nullopt;
    return f;
}

class serialOut final : public nodeBase {
public:
    explicit serialOut(nodeContext& c)
        : nodeBase(c),
          port_(c.io().openSerialOut(c.id(), c.text("device"), baud(c))),
          in_(c.eventIn("in")),
          value_(c.signalIn("value")),
          io_(c.io()) {
        c.keep(port_);
    }

    // A line per event; a control value alone sends nothing (§4.8).
    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        if (retiring()) return;
        for (const event& e : in_->events()) {
            const auto f = frameFor(e, value_, ctx);
            if (!f || !port_->queue.push(*f)) io_.countDrop();  // §7.3.3
        }
    }

private:
    std::shared_ptr<serialPort> port_;
    std::shared_ptr<const ePipe> in_;
    signal value_;
    ioHub& io_;
};

// --- Web (io.md §5–§7) -----------------------------------------------------------

class webIn final : public frameSource<webPort> {
public:
    explicit webIn(nodeContext& c)
        : frameSource(c,
                      c.io().openWebIn(portKind::webIn, c.id(),
                                       {.url = c.text("url"),
                                        .headers = c.textList("headers"),
                                        .fields = c.textList("fields"),
                                        .every = c.number("every"),
                                        .timeout = c.number("timeout")}),
                      "webIn") {}
};

class webStream final : public frameSource<webPort> {
public:
    explicit webStream(nodeContext& c)
        : frameSource(c,
                      c.io().openWebIn(portKind::webStream, c.id(),
                                       {.url = c.text("url"),
                                        .headers = c.textList("headers"),
                                        .fields = c.textList("fields"),
                                        .event = c.text("event")}),
                      "webStream") {}
};

class webSocket final : public frameSource<webPort> {
public:
    explicit webSocket(nodeContext& c)
        : frameSource(c,
                      c.io().openWebIn(portKind::webSocket, c.id(),
                                       {.url = c.text("url"),
                                        .headers = c.textList("headers"),
                                        .fields = c.textList("fields"),
                                        .hello = c.text("hello")}),
                      "webSocket") {}
};

// A request per event (io.md §7). The engine applies no rate limiting; a
// full queue drops the request, and :dropped counts the node's drops.
class webOut final : public nodeBase {
public:
    explicit webOut(nodeContext& c)
        : nodeBase(c),
          port_(c.io().openWebOut(c.id(), {.url = c.text("url"),
                                           .headers = c.textList("headers"),
                                           .fields = c.textList("fields"),
                                           .timeout = c.number("timeout"),
                                           .method = c.text("method")})),
          in_(c.eventIn("in")),
          value_(c.signalIn("value")),
          dropped_(c.controlOut("dropped")),
          io_(c.io()) {
        c.keep(port_);
    }

    // The count carries over, so :dropped never goes back.
    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        drops_ = static_cast<const webOut&>(outgoing).drops_;
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        if (!retiring()) {
            for (const event& e : in_->events()) {
                const auto f = frameFor(e, value_, ctx);
                if (f && port_->queue.push(*f)) continue;
                ++drops_;
                io_.countDrop();  // §7.3
            }
        }
        emitAll(dropped_, static_cast<float>(drops_));
    }

private:
    std::shared_ptr<webPort> port_;
    std::shared_ptr<const ePipe> in_;
    signal value_;
    controlOutput dropped_;
    ioHub& io_;
    std::uint64_t drops_ = 0;
};

} // namespace

std::span<const factoryEntry> deviceFactories() {
    static const auto factories = std::to_array<factoryEntry>({
        {.type = "audioIn", .make = make<audioIn>},
        {.type = "audioOut", .make = make<audioOut>},
        {.type = "midiIn", .make = make<midiIn>},
        {.type = "midiOut", .make = make<midiOut>},
        {.type = "oscIn", .make = make<oscIn>},
        {.type = "oscOut", .make = make<oscOut>},
        {.type = "serialIn", .make = make<serialIn>},
        {.type = "serialOut", .make = make<serialOut>},
        {.type = "webIn", .make = make<webIn>},
        {.type = "webStream", .make = make<webStream>},
        {.type = "webSocket", .make = make<webSocket>},
        {.type = "webOut", .make = make<webOut>},
    });
    return factories;
}

} // namespace primitives
