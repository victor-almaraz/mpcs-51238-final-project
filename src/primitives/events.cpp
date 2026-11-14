// Event generators (primitives.md §7.8).

#include "node.h"

#include <bitset>
#include <cmath>

namespace primitives {

namespace {

// MIDI channel parameter: 0 matches every channel; 1 to 16 select one.
bool channelMatches(int wanted, const event& e) noexcept { return wanted == 0 || channel(e) + 1 == wanted; }

// --- Events to a control value ------------------------------------------

// Folds a block's events into one value and writes it to every channel of
// a control pipe. Readers differ only in update().
template <typename Reader>
class eventReader final : public primitive {
public:
    explicit eventReader(nodeContext& c)
        : primitive(c.id(), c.type()),
          in_(c.eventIn("in")),
          out_(c.controlOut("out")),
          channels_(c.outputChannels("out")),
          reader_(c) {}

    void prepare(const prepareInfo&) override {}

    // Takes the outgoing reader's state, not its parameters (§9.2).
    void adoptState(primitive& outgoing) noexcept override { reader_.adopt(static_cast<eventReader&>(outgoing).reader_); }

    void process(const blockContext&) noexcept override {
        for (const event& e : in_->events()) reader_.update(e);
        if (const auto v = reader_.value()) {
            for (int ch = 0; ch < channels_; ++ch) emit(*out_, ch, *v);
        }
    }

private:
    const ePipe* in_;
    cPipe* out_;
    int channels_;
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
    float reference;
    int note = -1;
};

struct gateReader {
    explicit gateReader(nodeContext&) {}
    void update(const event& e) noexcept {
        if (e.type != eventType::noteOn && e.type != eventType::noteOff) return;
        held.set(static_cast<std::size_t>(channel(e) * 128 + data1(e)), e.type == eventType::noteOn);
    }
    std::optional<float> value() const noexcept { return held.any() ? 1.0f : 0.0f; }
    void adopt(const gateReader& o) noexcept { held = o.held; }
    std::bitset<16 * 128> held;
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
    int controller;
    int wanted;
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
    float range;
    int wanted;
    int bend = 8192;
};

// --- eventFilter ----------------------------------------------------------

class eventFilter final : public primitive {
public:
    explicit eventFilter(nodeContext& c)
        : primitive(c.id(), c.type()),
          kind_(c.choice("type")),
          channel_(static_cast<int>(c.number("channel"))),
          low_(static_cast<int>(c.number("low"))),
          high_(static_cast<int>(c.number("high"))),
          address_(c.text("address")),
          in_(c.eventIn("in")),
          out_(c.eventOut("out")) {}

    void prepare(const prepareInfo&) override {}

    void process(const blockContext&) noexcept override {
        for (const event& e : in_->events()) {
            if (passes(e) && keep(e)) out_->copyFrom(*in_, e);
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
        if (e.type == osc && !address_.empty() && oscAddress(in_->data(oscData(e))) != address_) return false;
        return true;
    }

    int kind_;
    int channel_;
    int low_;
    int high_;
    std::string address_;
    const ePipe* in_;
    ePipe* out_;
};

// --- voiceSplit -----------------------------------------------------------

class voiceSplit final : public primitive {
public:
    explicit voiceSplit(nodeContext& c)
        : primitive(c.id(), c.type()), in_(c.eventIn("in")), outs_(c.eventOuts("out")), voices_(outs_.size()) {}

    void prepare(const prepareInfo&) override {}

    void adoptState(primitive& outgoing) noexcept override {
        auto& o = static_cast<voiceSplit&>(outgoing);
        adoptIfSameSize(voices_, o.voices_);
        clock_ = o.clock_;
    }

    void process(const blockContext& ctx) noexcept override {
        for (const event& e : in_->events()) {
            if (e.type == eventType::noteOn) {
                noteOn(e);
            } else if (e.type == eventType::noteOff) {
                noteOff(e);
            } else if (passes(e)) {
                for (ePipe* out : outs_) out->copyFrom(*in_, e);
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
            for (std::size_t v = 1; v < voices_.size(); ++v) {
                if (voices_[v].started < voices_[chosen].started) chosen = v;
            }
            const voice& old = voices_[chosen];
            emit(*outs_[chosen], primitives::noteOff(e.sampleOffset, old.channel, old.note));
        }
        voices_[chosen] = {true, channel(e), data1(e), ++clock_};
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

    const ePipe* in_;
    std::vector<ePipe*> outs_;
    std::vector<voice> voices_;
    std::uint64_t clock_ = 0;
};

// --- metro ----------------------------------------------------------------

class metro final : public primitive {
public:
    explicit metro(nodeContext& c)
        : primitive(c.id(), c.type()), every_(c.number("every")), out_(c.eventOut("out")) {}

    void prepare(const prepareInfo&) override {}

    // The last trigger's beat, not its index, so a new `every` applies at once.
    void adoptState(primitive& outgoing) noexcept override { last_ = static_cast<metro&>(outgoing).last_; }

    // Triggers at every multiple of `every` within this block, with sample
    // offsets (§7.8.1).
    void process(const blockContext& ctx) noexcept override {
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
    double every_;
    ePipe* out_;
    double last_ = -std::numeric_limits<double>::infinity();
};

// --- sequencer --------------------------------------------------------------

class sequencer final : public primitive {
public:
    explicit sequencer(nodeContext& c)
        : primitive(c.id(), c.type()),
          order_(c.choice("order")),
          in_(c.eventIn("in")),
          out_(c.controlOut("out")),
          events_(c.eventOut("events")),
          channels_(c.outputChannels("out")),
          random_(c.seed()) {
        for (double v : c.list("values")) values_.push_back(static_cast<float>(v));
        if (values_.empty()) c.fail("values must not be empty");
    }

    void prepare(const prepareInfo&) override {}

    void adoptState(primitive& outgoing) noexcept override {
        auto& o = static_cast<sequencer&>(outgoing);
        index_ = o.index_;
        direction_ = o.direction_;
        random_ = o.random_;
    }

    void process(const blockContext&) noexcept override {
        for (const event& e : in_->events()) {
            if (e.type == eventType::noteOff) continue;
            step();
            if (events_) emit(*events_, valueEvent(e.sampleOffset, current()));
        }
        if (out_ && index_ >= 0) {
            for (int ch = 0; ch < channels_; ++ch) emit(*out_, ch, current());
        }
    }

private:
    float current() const noexcept { return values_[static_cast<std::size_t>(std::max(index_, 0L))]; }

    void step() noexcept {
        const auto n = static_cast<long>(values_.size());
        switch (order_) {
        case 0: index_ = (index_ + 1) % n; break;
        case 1: index_ = index_ <= 0 ? n - 1 : index_ - 1; break;
        case 2:  // ping-pong without repeating the ends
            if (n == 1) {
                index_ = 0;
            } else {
                if (index_ + direction_ < 0 || index_ + direction_ >= n) direction_ = -direction_;
                index_ += direction_;
            }
            break;
        default: index_ = static_cast<long>(random_.next() % static_cast<std::uint64_t>(n));
        }
    }

    int order_;
    const ePipe* in_;
    cPipe* out_;
    ePipe* events_;
    int channels_;
    rng random_;
    std::vector<float> values_;
    long index_ = -1;
    long direction_ = 1;
};

// --- chance -----------------------------------------------------------------

class chance final : public primitive {
public:
    explicit chance(nodeContext& c)
        : primitive(c.id(), c.type()),
          probability_(c.param("probability")),
          in_(c.eventIn("in")),
          out_(c.eventOut("out")),
          random_(c.seed()) {}

    void prepare(const prepareInfo&) override {}

    void adoptState(primitive& outgoing) noexcept override { random_ = static_cast<chance&>(outgoing).random_; }

    void process(const blockContext&) noexcept override {
        const float p = probability_.value();
        for (const event& e : in_->events()) {
            // Draw for every event, so the sequence does not depend on fades.
            if (random_.uniform() < p && passes(e)) out_->copyFrom(*in_, e);
        }
    }

private:
    signal probability_;
    const ePipe* in_;
    ePipe* out_;
    rng random_;
};

// --- threshold ----------------------------------------------------------------

class threshold final : public primitive {
public:
    explicit threshold(nodeContext& c)
        : primitive(c.id(), c.type()),
          in_(c.signalIn("in")),
          level_(c.param("level")),
          direction_(c.choice("direction")),
          out_(c.eventOut("out")) {}

    void prepare(const prepareInfo&) override {}

    void adoptState(primitive& outgoing) noexcept override { previous_ = static_cast<threshold&>(outgoing).previous_; }

    void process(const blockContext& ctx) noexcept override {
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
        if (!std::isnan(previous_)) {
            const bool up = previous_ < level && x >= level;
            const bool down = previous_ > level && x <= level;
            if ((direction_ == 0 && up) || (direction_ == 1 && down) || (direction_ == 2 && (up || down))) {
                emit(*out_, trigger(offset));
            }
        }
        previous_ = x;
    }

    signal in_;
    signal level_;
    int direction_;
    ePipe* out_;
    float previous_ = NAN;
};

// --- sampleAndHold --------------------------------------------------------------

class sampleAndHold final : public primitive {
public:
    explicit sampleAndHold(nodeContext& c)
        : primitive(c.id(), c.type()),
          trigger_(c.eventIn("trigger")),
          in_(c.signalIn("in")),
          out_(c.controlOut("out")),
          channels_(c.outputChannels("out")) {}

    void prepare(const prepareInfo&) override {}

    void process(const blockContext& ctx) noexcept override {
        const auto events = trigger_->events();
        if (events.empty()) return;
        for (int ch = 0; ch < channels_; ++ch) {
            float held = 0.0f;
            if (in_.isAudio()) {
                const auto x = in_.block(ch, ctx.numSamples);
                held = x.empty() ? 0.0f : x[std::min<std::size_t>(events.back().sampleOffset, x.size() - 1)];
            } else {
                held = in_.value(ch);
            }
            emit(*out_, ch, held);
        }
    }

private:
    const ePipe* trigger_;
    signal in_;
    cPipe* out_;
    int channels_;
};

constexpr factoryEntry factories[] = {
    {"noteToFreq", make<eventReader<noteToFreqReader>>},
    {"gate", make<eventReader<gateReader>>},
    {"velocity", make<eventReader<velocityReader>>},
    {"controlChange", make<eventReader<controlChangeReader>>},
    {"pitchBend", make<eventReader<pitchBendReader>>},
    {"eventFilter", make<eventFilter>},
    {"voiceSplit", make<voiceSplit>},
    {"metro", make<metro>},
    {"sequencer", make<sequencer>},
    {"chance", make<chance>},
    {"threshold", make<threshold>},
    {"sampleAndHold", make<sampleAndHold>},
};

} // namespace

std::span<const factoryEntry> eventFactories() { return factories; }

} // namespace primitives
