// Event generators (primitives.md §7.8).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <espresso/primitives/event.h>
#include <espresso/primitives/pipe.h>
#include <espresso/primitives/primitive.h>
#include <espresso/primitives/realtime.h>

#include "node.h"

namespace primitives {

namespace {

// MIDI channel parameter: 0 matches every channel; 1 to 16 select one.
bool channelMatches(int wanted, const event& e) noexcept { return wanted == 0 || channel(e) + 1 == wanted; }

// --- Events to a control value ------------------------------------------

// Folds a block's events into one value and writes it to every channel of
// a control pipe. Readers differ only in update().
template <typename Reader>
class eventReader final : public nodeBase {
public:
    explicit eventReader(nodeContext& c)
        : nodeBase(c), in_(c.eventIn("in")), out_(c.controlOut("out")), reader_(c) {}

    // Takes the outgoing reader's state, not its parameters (§9.2).
    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        reader_.adopt(static_cast<eventReader&>(outgoing).reader_);
    }

    void process(const blockContext&) noexcept ESPRESSO_NONBLOCKING override {
        for (const event& e : in_->events()) reader_.update(e);
        if (const auto v = reader_.value()) emitAll(out_, *v);
    }

private:
    std::shared_ptr<const ePipe> in_;
    controlOutput out_;
    Reader reader_;
};

// Each reader keeps the raw state (the latest note, controller value, or
// bend) and computes its output from its own parameters, so a swap that
// changes a parameter takes effect at once.
struct noteToFreqReader {
    explicit noteToFreqReader(nodeContext& c) : reference(static_cast<float>(c.number("reference"))) {}
    void update(const event& e) noexcept {
        if (e.type == eventType::noteOn) note = data1(e);
    }
    std::optional<float> value() const noexcept {
        if (note < 0) return std::nullopt;
        return reference * std::exp2(static_cast<float>(note - 69) / 12.0f);
    }
    void adopt(const noteToFreqReader& o) noexcept { note = o.note; }
    float reference = 440.0f;
    int note = -1;
};

struct gateReader {
    explicit gateReader(nodeContext&) {}
    void update(const event& e) noexcept { held.update(e); }
    std::optional<float> value() const noexcept { return held.any() ? 1.0f : 0.0f; }
    void adopt(const gateReader& o) noexcept { held = o.held; }
    heldNotes held;
};

struct velocityReader {
    explicit velocityReader(nodeContext&) {}
    void update(const event& e) noexcept {
        if (e.type == eventType::noteOn) velocity = data2(e);
    }
    std::optional<float> value() const noexcept {
        if (velocity < 0) return std::nullopt;
        return static_cast<float>(velocity) / 127.0f;
    }
    void adopt(const velocityReader& o) noexcept { velocity = o.velocity; }
    int velocity = -1;
};

struct controlChangeReader {
    explicit controlChangeReader(nodeContext& c)
        : controller(static_cast<int>(c.number("cc"))), wanted(static_cast<int>(c.number("channel"))) {}
    void update(const event& e) noexcept {
        if (e.type == eventType::controlChange && data1(e) == controller && channelMatches(wanted, e)) raw = data2(e);
    }
    std::optional<float> value() const noexcept {
        if (raw < 0) return std::nullopt;
        return static_cast<float>(raw) / 127.0f;
    }
    // A different controller or channel starts without a value.
    void adopt(const controlChangeReader& o) noexcept {
        if (o.controller == controller && o.wanted == wanted) raw = o.raw;
    }
    int controller = 0;
    int wanted = 0;
    int raw = -1;
};

struct pitchBendReader {
    explicit pitchBendReader(nodeContext& c)
        : range(static_cast<float>(c.number("range"))), wanted(static_cast<int>(c.number("channel"))) {}
    void update(const event& e) noexcept {
        if (e.type == eventType::pitchBend && channelMatches(wanted, e)) bend = bendValue(e);
    }
    std::optional<float> value() const noexcept { return static_cast<float>(bend - 8192) / 8192.0f * range; }
    void adopt(const pitchBendReader& o) noexcept {
        if (o.wanted == wanted) bend = o.bend;
    }
    float range = 2.0f;
    int wanted = 0;
    int bend = 8192;
};

// --- eventFilter ----------------------------------------------------------

class eventFilter final : public nodeBase {
public:
    explicit eventFilter(nodeContext& c)
        : nodeBase(c),
          kind_(c.choice("type")),
          channel_(static_cast<int>(c.number("channel"))),
          low_(static_cast<int>(c.number("low"))),
          high_(static_cast<int>(c.number("high"))),
          address_(c.text("address")),
          in_(c.eventIn("in")),
          out_(c.eventOut("out")) {}

    void process(const blockContext&) noexcept ESPRESSO_NONBLOCKING override {
        for (const event& e : in_->events()) {
            if (keep(e)) forward(*out_, *in_, e);
        }
    }

private:
    bool keep(const event& e) const noexcept {
        using enum eventType;
        const bool note = e.type == noteOn || e.type == noteOff || e.type == polyPressure;
        switch (kind_) {
        case 1: if (e.type != noteOn && e.type != noteOff) return false; break;
        case 2: if (e.type != controlChange) return false; break;
        case 3: if (e.type != pitchBend) return false; break;
        case 4: if (e.type != programChange) return false; break;
        case 5: if (e.type != channelPressure && e.type != polyPressure) return false; break;
        case 6: if (e.type != trigger) return false; break;
        case 7: if (e.type != value) return false; break;
        case 8: if (e.type != osc) return false; break;
        default: break;
        }
        if (isMidi(e.type) && !channelMatches(channel_, e)) return false;
        if (note && (data1(e) < low_ || data1(e) > high_)) return false;
        if (e.type == osc && !address_.empty() && !oscAddressIs(in_->data(oscData(e)), address_)) return false;
        return true;
    }

    int kind_ = 0;
    int channel_ = 0;
    int low_ = 0;
    int high_ = 127;
    std::string address_;
    std::shared_ptr<const ePipe> in_;
    std::shared_ptr<ePipe> out_;
};

// --- voiceSplit -----------------------------------------------------------

class voiceSplit final : public nodeBase {
public:
    explicit voiceSplit(nodeContext& c)
        : nodeBase(c), in_(c.eventIn("in")), outs_(c.eventOuts("out")), voices_(outs_.size()) {}

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        auto& o = static_cast<voiceSplit&>(outgoing);
        adoptIfSameSize(voices_, o.voices_);
        clock_ = o.clock_;
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        for (const event& e : in_->events()) {
            if (e.type == eventType::noteOn) {
                noteOn(e);
            } else if (e.type == eventType::noteOff) {
                noteOff(e);
            } else {
                for (const auto& out : outs_) forward(*out, *in_, e);
            }
        }
        // A retiring split ends its held voices, whose note-offs the next
        // plan's split could not route.
        if (retiring() && finalBlock()) {
            for (std::size_t v = 0; v < voices_.size(); ++v) {
                voice& x = voices_[v];
                if (!x.active) continue;
                x.active = false;
                emit(*outs_[v], primitives::noteOff(static_cast<std::uint32_t>(ctx.numSamples - 1), x.channel, x.note));
            }
        }
    }

private:
    struct voice {
        bool active = false;
        int channel = 0;
        int note = 0;
        std::uint64_t started = 0;
    };

    void noteOn(const event& e) noexcept {
        std::size_t chosen = 0;
        bool found = false;
        // A repeated note retriggers its voice, so one note-off ends it.
        for (std::size_t v = 0; v < voices_.size() && !found; ++v) {
            const voice& x = voices_[v];
            if (x.active && x.channel == channel(e) && x.note == data1(e)) chosen = v, found = true;
        }
        for (std::size_t v = 0; v < voices_.size() && !found; ++v) {
            if (!voices_[v].active) chosen = v, found = true;
        }
        if (!found) {  // steal the oldest (§7.8.3)
            chosen = static_cast<std::size_t>(std::ranges::min_element(voices_, {}, &voice::started) -
                                              voices_.begin());
            const voice& old = voices_[chosen];
            emit(*outs_[chosen], primitives::noteOff(e.sampleOffset, old.channel, old.note));
        }
        voices_[chosen] = {.active = true, .channel = channel(e), .note = data1(e), .started = ++clock_};
        emit(*outs_[chosen], e);
    }

    void noteOff(const event& e) noexcept {
        for (std::size_t v = 0; v < voices_.size(); ++v) {
            voice& x = voices_[v];
            if (x.active && x.note == data1(e) && x.channel == channel(e)) {
                x.active = false;
                emit(*outs_[v], e);
                return;
            }
        }
    }

    std::shared_ptr<const ePipe> in_;
    std::vector<std::shared_ptr<ePipe>> outs_;
    std::vector<voice> voices_;
    std::uint64_t clock_ = 0;
};

// --- triggerNotes -----------------------------------------------------------

// A MIDI number, 0 to 127, from @p x: below 0 or NaN is 0.
int midiNumber(float x) noexcept { return x > 0.0f ? static_cast<int>(std::lround(std::min(x, 127.0f))) : 0; }

// A note for each trigger or value event: a note-on at the event, and a
// note-off `length` seconds later. A trigger plays `pitch`; a value
// event, such as a sequencer's, plays its own number unless `pitch` is
// given. Pitch, velocity, and length follow their pipes once a block.
class triggerNotes final : public nodeBase {
public:
    explicit triggerNotes(nodeContext& c)
        : nodeBase(c),
          in_(c.eventIn("in")),
          out_(c.eventOut("out")),
          pitch_(c.param("pitch")),
          velocity_(c.param("velocity")),
          length_(c.param("length")),
          pitchGiven_(c.given("pitch")) {}

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        const auto& o = static_cast<triggerNotes&>(outgoing);
        sounding_ = o.sounding_;
        count_ = o.count_;
        clock_ = o.clock_;
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const auto n = static_cast<std::int64_t>(ctx.numSamples);
        const int pitch = midiNumber(pitch_.value());
        const int velocity = std::max(1, midiNumber(velocity_.value() * 127.0f));
        const auto length = std::max<std::int64_t>(1, std::llround(length_.value() * ctx.sampleRate));
        for (const event& e : in_->events()) {
            if (e.type != eventType::trigger && e.type != eventType::value) continue;
            const auto at = static_cast<std::int64_t>(e.sampleOffset);
            endDue(at);
            // A retiring node starts no notes: their note-ons would not pass.
            if (retiring()) continue;
            const int note = e.type == eventType::value && !pitchGiven_ ? midiNumber(number(e)) : pitch;
            // A sounding note of this pitch ends first, so each note-off ends
            // one note and at most one note of a pitch sounds.
            if (const auto same = std::ranges::find(sounding(), note, &sound::note); same != sounding().end())
                end(index(same), at);
            if (count_ == sounding_.size())
                end(index(std::ranges::min_element(sounding(), {}, &sound::started)), at);
            emit(*out_, noteOn(static_cast<std::uint32_t>(at), 0, note, velocity));
            sounding_[count_++] = {.note = note, .due = at + length, .started = ++clock_};
        }
        endDue(n - 1);
        // A retiring node ends its notes, whose note-offs no node would send.
        if (retiring() && finalBlock()) {
            while (count_ > 0) end(0, n - 1);
        }
        for (auto& s : sounding()) s.due -= n;
    }

private:
    struct sound {
        int note = 0;
        std::int64_t due = 0;  // sample of its note-off, from this block's start
        std::uint64_t started = 0;
    };

    std::span<sound> sounding() noexcept { return std::span(sounding_).first(count_); }
    std::size_t index(std::span<sound>::iterator it) noexcept {
        return static_cast<std::size_t>(it - sounding().begin());
    }

    // Ends every note due at or before sample @p limit of this block, each
    // at its due sample.
    void endDue(std::int64_t limit) noexcept {
        for (std::size_t i = 0; i < count_;) {
            if (sounding_[i].due <= limit) {
                end(i, sounding_[i].due);
            } else {
                ++i;
            }
        }
    }

    // Sends note @p i's note-off at sample @p at, 0 or later, and forgets it.
    void end(std::size_t i, std::int64_t at) noexcept {
        emit(*out_, noteOff(static_cast<std::uint32_t>(at), 0, sounding_[i].note));
        sounding_[i] = sounding_[--count_];
    }

    std::shared_ptr<const ePipe> in_;
    std::shared_ptr<ePipe> out_;
    signal pitch_;
    signal velocity_;
    signal length_;
    bool pitchGiven_ = false;
    std::array<sound, 32> sounding_{};  // at most 32 notes overlap; a 33rd ends the oldest
    std::size_t count_ = 0;
    std::uint64_t clock_ = 0;
};

// --- metro ----------------------------------------------------------------

class metro final : public nodeBase {
public:
    explicit metro(nodeContext& c) : nodeBase(c), every_(c.number("every")), out_(c.eventOut("out")) {}

    // The last trigger's beat, not its index, so a new `every` applies at once.
    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        last_ = static_cast<metro&>(outgoing).last_;
    }

    // Triggers at every multiple of `every` within this block, with sample
    // offsets (§7.8.1).
    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const double perSample = ctx.beatsPerSample();
        if (perSample <= 0.0) return;
        // The beat clock accumulates rounding error; a beat within a
        // millionth of a sample of the block end belongs to the next block.
        const double start = ctx.beat;
        const double end = start + perSample * (ctx.numSamples - 1e-6);
        const double tolerance = perSample * 1e-3;
        for (auto k = static_cast<std::int64_t>(std::ceil(start / every_ - 1e-9)); static_cast<double>(k) * every_ < end; ++k) {
            const double beat = static_cast<double>(k) * every_;
            if (beat <= last_ + tolerance) continue;  // already emitted at the previous block's end
            last_ = beat;
            const double offset = std::floor((beat - start) / perSample);
            emit(*out_, trigger(static_cast<std::uint32_t>(std::clamp(offset, 0.0, ctx.numSamples - 1.0))));
        }
    }

private:
    double every_ = 1.0;
    std::shared_ptr<ePipe> out_;
    double last_ = -std::numeric_limits<double>::infinity();
};

// --- sequencer --------------------------------------------------------------

class sequencer final : public nodeBase {
public:
    explicit sequencer(nodeContext& c)
        : nodeBase(c),
          order_(c.choice("order")),
          in_(c.eventIn("in")),
          out_(c.controlOut("out")),
          events_(c.eventOut("events")),
          random_(c.seed()) {
        for (double v : c.list("values")) values_.push_back(static_cast<float>(v));
        if (values_.empty()) c.fail("values must not be empty");
    }

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        const auto& o = static_cast<const sequencer&>(outgoing);
        index_ = o.index_;
        direction_ = o.direction_;
        random_ = o.random_;
    }

    void process(const blockContext&) noexcept ESPRESSO_NONBLOCKING override {
        for (const event& e : in_->events()) {
            if (e.type == eventType::noteOff) continue;
            step();
            if (events_) emit(*events_, valueEvent(e.sampleOffset, current()));
        }
        if (index_) emitAll(out_, current());
    }

private:
    float current() const noexcept { return values_[index_.value_or(0)]; }

    void step() noexcept {
        const std::size_t n = values_.size();
        switch (order_) {
        case 0: index_ = index_ ? (*index_ + 1) % n : 0; break;
        case 1: index_ = !index_ || *index_ == 0 ? n - 1 : *index_ - 1; break;
        case 2:  // ping-pong without repeating the ends
            if (!index_ || n == 1) {
                index_ = 0;
            } else {
                if ((direction_ < 0 && *index_ == 0) || (direction_ > 0 && *index_ + 1 == n))
                    direction_ = -direction_;
                index_ = direction_ > 0 ? *index_ + 1 : *index_ - 1;
            }
            break;
        default: index_ = static_cast<std::size_t>(random_.next() % n);
        }
    }

    int order_ = 0;
    std::shared_ptr<const ePipe> in_;
    controlOutput out_;
    std::shared_ptr<ePipe> events_;
    rng random_;
    std::vector<float> values_;
    // The current value's index; none before the first trigger.
    std::optional<std::size_t> index_;
    int direction_ = 1;
};

// --- chance -----------------------------------------------------------------

class chance final : public nodeBase {
public:
    explicit chance(nodeContext& c)
        : nodeBase(c),
          probability_(c.param("probability")),
          in_(c.eventIn("in")),
          out_(c.eventOut("out")),
          random_(c.seed()) {}

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        random_ = static_cast<chance&>(outgoing).random_;
    }

    void process(const blockContext&) noexcept ESPRESSO_NONBLOCKING override {
        const float p = probability_.value();
        for (const event& e : in_->events()) {
            // Draw for every event, so the sequence does not depend on fades.
            if (random_.uniform() < p) forward(*out_, *in_, e);
        }
    }

private:
    signal probability_;
    std::shared_ptr<const ePipe> in_;
    std::shared_ptr<ePipe> out_;
    rng random_;
};

// --- threshold ----------------------------------------------------------------

class threshold final : public nodeBase {
public:
    explicit threshold(nodeContext& c)
        : nodeBase(c),
          in_(c.signalIn("in")),
          level_(c.param("level")),
          direction_(c.choice("direction")),
          out_(c.eventOut("out")) {}

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        previous_ = static_cast<threshold&>(outgoing).previous_;
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const float level = level_.value();
        if (!in_.isAudio()) {
            check(in_.value(), level, 0);
            return;
        }
        const auto x = in_.block(0, ctx.numSamples);
        for (std::size_t i = 0; i < x.size(); ++i) check(x[i], level, static_cast<std::uint32_t>(i));
    }

private:
    void check(float x, float level, std::uint32_t offset) noexcept {
        if (previous_) {
            const bool up = *previous_ < level && x >= level;
            const bool down = *previous_ > level && x <= level;
            if ((direction_ == 0 && up) || (direction_ == 1 && down) || (direction_ == 2 && (up || down))) {
                emit(*out_, trigger(offset));
            }
        }
        previous_ = x;
    }

    signal in_;
    signal level_;
    int direction_ = 0;
    std::shared_ptr<ePipe> out_;
    // The input's last value; none before the first.
    std::optional<float> previous_;
};

// --- sampleAndHold --------------------------------------------------------------

class sampleAndHold final : public nodeBase {
public:
    explicit sampleAndHold(nodeContext& c)
        : nodeBase(c), trigger_(c.eventIn("trigger")), in_(c.signalIn("in")), out_(c.controlOut("out")) {}

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const auto events = trigger_->events();
        if (events.empty()) return;
        for (int ch = 0; ch < out_.channels; ++ch) {
            float held = 0.0f;
            if (in_.isAudio()) {
                const auto x = in_.block(ch, ctx.numSamples);
                held = x.empty() ? 0.0f : x[std::min<std::size_t>(events.back().sampleOffset, x.size() - 1)];
            } else {
                held = in_.value(ch);
            }
            emit(*out_.pipe, ch, held);
        }
    }

private:
    std::shared_ptr<const ePipe> trigger_;
    signal in_;
    controlOutput out_;
};

} // namespace

std::span<const factoryEntry> eventFactories() {
    static const auto factories = std::to_array<factoryEntry>({
        {.type = "noteToFreq", .make = make<eventReader<noteToFreqReader>>},
        {.type = "gate", .make = make<eventReader<gateReader>>},
        {.type = "velocity", .make = make<eventReader<velocityReader>>},
        {.type = "controlChange", .make = make<eventReader<controlChangeReader>>},
        {.type = "pitchBend", .make = make<eventReader<pitchBendReader>>},
        {.type = "eventFilter", .make = make<eventFilter>},
        {.type = "voiceSplit", .make = make<voiceSplit>},
        {.type = "triggerNotes", .make = make<triggerNotes>},
        {.type = "metro", .make = make<metro>},
        {.type = "sequencer", .make = make<sequencer>},
        {.type = "chance", .make = make<chance>},
        {.type = "threshold", .make = make<threshold>},
        {.type = "sampleAndHold", .make = make<sampleAndHold>},
    });
    return factories;
}

} // namespace primitives
