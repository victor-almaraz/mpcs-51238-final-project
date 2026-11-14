// Sources and sinks: audio, MIDI, and OSC (primitives.md §7.2, §7.3).
// MIDI and OSC data cross to the I/O layer through ports (io.h).

#include "node.h"

#include <cmath>

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

// --- Audio ----------------------------------------------------------------

class audioIn final : public primitive {
public:
    explicit audioIn(nodeContext& c)
        : primitive(c.id(), c.type()),
          first_(static_cast<int>(c.number("channel"))),
          out_(c.audioOut("out")),
          channels_(c.outputChannels("out")) {}

    void prepare(const prepareInfo&) override {}

    void process(const blockContext& ctx) noexcept override {
        const deviceBuffers* d = ctx.device;
        if (!d || !d->inputs) return;
        const auto n = static_cast<std::size_t>(ctx.numSamples);
        for (int ch = 0; ch < channels_; ++ch) {
            const int source = first_ + ch;
            if (source >= d->numInputs || !d->inputs[source]) break;
            emit(*out_, ch, {d->inputs[source] + ctx.deviceOffset, n});
        }
    }

private:
    int first_;
    aPipe* out_;
    int channels_;
};

class audioOut final : public primitive {
public:
    explicit audioOut(nodeContext& c)
        : primitive(c.id(), c.type()), first_(static_cast<int>(c.number("channel"))), in_(c.audioIn("in")) {}

    void prepare(const prepareInfo&) override {}

    void process(const blockContext& ctx) noexcept override {
        const deviceBuffers* d = ctx.device;
        if (!d || !d->outputs) return;
        for (int ch = 0; ch < in_->channels(); ++ch) {
            const int target = first_ + ch;
            if (target >= d->numOutputs || !d->outputs[target]) break;
            emitDevice(d->outputs[target] + ctx.deviceOffset, in_->read(ch));
        }
    }

private:
    int first_;
    const aPipe* in_;
};

// --- MIDI -----------------------------------------------------------------

class midiIn final : public primitive {
public:
    explicit midiIn(nodeContext& c)
        : primitive(c.id(), c.type()),
          port_(c.io().openMidiIn(c.text("device"))),
          channel_(static_cast<int>(c.number("channel"))),
          out_(c.eventOut("out")) {
        c.keep(port_);
    }

    void prepare(const prepareInfo&) override {}

    void process(const blockContext& ctx) noexcept override {
        midiPacket p;
        while (port_->queue.pop(p)) {
            const auto e = fromMidi({p.bytes.data(), p.size}, offsetFor(p.time, ctx));
            if (e && (channel_ == 0 || primitives::channel(*e) + 1 == channel_)) emit(*out_, *e);
        }
    }

private:
    std::unique_ptr<midiPort> port_;
    int channel_;
    ePipe* out_;
};

class midiOut final : public primitive {
public:
    explicit midiOut(nodeContext& c)
        : primitive(c.id(), c.type()),
          port_(c.io().openMidiOut(c.text("device"))),
          channel_(static_cast<int>(c.number("channel"))),
          in_(c.eventIn("in")),
          io_(c.io()) {
        c.keep(port_);
    }

    void prepare(const prepareInfo&) override {}

    // A retiring sink sends nothing, so a fade does not double the notes.
    void process(const blockContext& ctx) noexcept override {
        if (retiring()) return;
        for (const event& e : in_->events()) {
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
    std::unique_ptr<midiPort> port_;
    int channel_;
    const ePipe* in_;
    ioHub& io_;
};

// --- OSC ------------------------------------------------------------------

class oscIn final : public primitive {
public:
    explicit oscIn(nodeContext& c)
        : primitive(c.id(), c.type()),
          port_(c.io().openOscIn(udpPort(c))),
          address_(c.text("address")),
          out_(c.eventOut("out")),
          value_(c.controlOut("value")),
          channels_(c.outputChannels("value")) {
        if (!out_ && !value_) c.fail("oscIn needs an out or value output");
        c.keep(port_);
    }

    void prepare(const prepareInfo&) override {}

    void process(const blockContext& ctx) noexcept override {
        oscPacket p;
        while (port_->queue.pop(p)) {
            const std::span<const std::byte> message(p.bytes.data(), p.size);
            if (!address_.empty() && oscAddress(message) != address_) continue;
            if (out_ && !retiring()) out_->writeOsc(offsetFor(p.time, ctx), message);
            if (value_) {
                if (const auto v = oscFirstNumber(message)) {
                    for (int ch = 0; ch < channels_; ++ch) emit(*value_, ch, *v);
                }
            }
        }
    }

private:
    std::unique_ptr<oscPort> port_;
    std::string address_;
    ePipe* out_;
    cPipe* value_;
    int channels_;
};

class oscOut final : public primitive {
public:
    explicit oscOut(nodeContext& c)
        : primitive(c.id(), c.type()),
          port_(c.io().openOscOut(udpPort(c))),
          address_(c.text("address")),
          in_(c.eventIn("in")),
          value_(c.hasInput("value") ? c.signalIn("value") : signal{}),
          hasValue_(c.hasInput("value")),
          io_(c.io()) {
        if (!in_ && !hasValue_) c.fail("oscOut needs an in or value input");
        if (hasValue_ && address_.empty()) c.fail("address must not be empty");
        c.keep(port_);
    }

    void prepare(const prepareInfo&) override {}

    // The last value sent, only when it was sent to the same place.
    void adoptState(primitive& outgoing) noexcept override {
        auto& o = static_cast<oscOut&>(outgoing);
        if (o.address_ == address_ && o.port_->udpPort() == port_->udpPort()) last_ = o.last_;
    }

    void process(const blockContext& ctx) noexcept override {
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
                std::copy(data.begin(), data.end(), p.bytes.begin());
                p.size = static_cast<std::uint16_t>(data.size());
                p.time = timeFor(e.sampleOffset, ctx);
                send(p);
            }
        }
        // A control value is sent when it changes.
        if (hasValue_) {
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

    std::unique_ptr<oscPort> port_;
    std::string address_;
    const ePipe* in_;
    signal value_;
    bool hasValue_;
    ioHub& io_;
    float last_ = NAN;
};

constexpr factoryEntry factories[] = {
    {"audioIn", make<audioIn>},
    {"audioOut", make<audioOut>},
    {"midiIn", make<midiIn>},
    {"midiOut", make<midiOut>},
    {"oscIn", make<oscIn>},
    {"oscOut", make<oscOut>},
};

} // namespace

std::span<const factoryEntry> deviceFactories() { return factories; }

} // namespace primitives
