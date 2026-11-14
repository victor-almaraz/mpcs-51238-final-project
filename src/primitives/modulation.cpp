// Envelopes and modulation: adsr, lfo, smoother (primitives.md §7.6).

#include "node.h"

#include <cmath>

namespace primitives {

namespace {

// A primitive whose output is an audio or a control pipe.
class modulator : public primitive {
public:
    using primitive::primitive;

protected:
    // Writes @p block to every channel of an audio output, or its last
    // value to every channel of a control output.
    void writeOut(const signalOut& out, std::span<const float> block) noexcept {
        for (int ch = 0; ch < out.channels; ++ch) {
            if (out.audio) {
                emit(*out.audio, ch, block);
            } else if (!block.empty()) {
                emit(*out.control, ch, block.back());
            }
        }
    }
};

// --- adsr ---------------------------------------------------------------

class adsr final : public modulator {
public:
    explicit adsr(nodeContext& c)
        : modulator(c.id(), c.type()),
          attack_(c.param("attack")),
          decay_(c.param("decay")),
          sustain_(c.param("sustain")),
          release_(c.param("release")),
          gain_(c.param("gain")),
          in_(c.signalIn("in")),
          hasIn_(c.hasInput("in")),
          gate_(c.eventIn("gate")),
          out_(c.signalOutput("out")) {
        if (hasIn_ && !out_.audio) c.fail("with an input, the output must be an audio pipe");
        // Without a gate the envelope starts at once and sustains.
        if (!gate_) stage_ = stage::attack;
    }

    void prepare(const prepareInfo& info) override {
        sampleRate_ = info.sampleRate;
        env_.assign(static_cast<std::size_t>(info.blockSize), 0.0f);
        buffer_.assign(env_.size(), 0.0f);
    }

    void adoptState(primitive& outgoing) noexcept override {
        auto& o = static_cast<adsr&>(outgoing);
        stage_ = o.stage_;
        level_ = o.level_;
        releaseStep_ = o.releaseStep_;
        oneShot_ = o.oneShot_;
    }

    void process(const blockContext& ctx) noexcept override {
        const auto n = static_cast<std::size_t>(ctx.numSamples);
        const double sr = sampleRate_;
        const double attackStep = 1.0 / std::max(attack_.value() * sr, 1.0);
        const double decayTime = std::max(decay_.value() * sr, 1.0);
        const double sustain = std::clamp(static_cast<double>(sustain_.value()), 0.0, 1.0);
        releaseSamples_ = std::max(release_.value() * sr, 1.0);
        const auto events = gate_ ? gate_->events() : std::span<const event>{};
        std::size_t next = 0;
        for (std::size_t i = 0; i < n; ++i) {
            for (; next < events.size() && events[next].sampleOffset <= i; ++next) onEvent(events[next]);
            switch (stage_) {
            case stage::idle: break;
            case stage::attack:
                level_ += attackStep;
                if (level_ >= 1.0) {
                    level_ = 1.0;
                    stage_ = stage::decay;
                }
                break;
            case stage::decay:
                level_ -= (1.0 - sustain) / decayTime;
                if (level_ <= sustain) {
                    level_ = sustain;
                    stage_ = stage::sustain;
                    if (oneShot_) startRelease();
                }
                break;
            case stage::sustain: level_ = sustain; break;
            case stage::release:
                level_ -= releaseStep_;
                if (level_ <= 0.0) {
                    level_ = 0.0;
                    stage_ = stage::idle;
                }
                break;
            }
            env_[i] = static_cast<float>(level_) * gain_.value();
        }
        if (!hasIn_) {
            writeOut(out_, std::span(env_).first(n));
            return;
        }
        for (int ch = 0; ch < out_.channels; ++ch) {
            const auto x = in_.block(ch, ctx.numSamples);
            for (std::size_t i = 0; i < n; ++i) buffer_[i] = x[i] * env_[i];
            emit(*out_.audio, ch, std::span(buffer_).first(n));
        }
    }

private:
    enum class stage : std::uint8_t { idle, attack, decay, sustain, release };

    void onEvent(const event& e) noexcept {
        switch (e.type) {
        case eventType::noteOn:
            stage_ = stage::attack;
            oneShot_ = false;
            break;
        case eventType::trigger:
            stage_ = stage::attack;
            oneShot_ = true;
            break;
        case eventType::noteOff:
            if (stage_ != stage::idle) startRelease();
            break;
        default: break;
        }
    }

    void startRelease() noexcept {
        stage_ = stage::release;
        oneShot_ = false;
        releaseStep_ = level_ / releaseSamples_;
    }

    signal attack_;
    signal decay_;
    signal sustain_;
    signal release_;
    signal gain_;
    signal in_;
    bool hasIn_;
    std::shared_ptr<const ePipe> gate_;
    signalOut out_;
    double sampleRate_ = 48000.0;
    double releaseSamples_ = 1.0;
    stage stage_ = stage::idle;
    double level_ = 0.0;
    double releaseStep_ = 0.0;
    bool oneShot_ = false;
    std::vector<float> env_;
    std::vector<float> buffer_;
};

// --- lfo ----------------------------------------------------------------

class lfo final : public modulator {
public:
    explicit lfo(nodeContext& c)
        : modulator(c.id(), c.type()),
          shape_(c.choice("shape")),
          rate_(c.param("rate")),
          phase_(c.param("phase")),
          depth_(c.param("depth")),
          offset_(c.param("offset")),
          out_(c.signalOutput("out")),
          random_(c.seed()) {
        held_ = random_.bipolar();
    }

    void prepare(const prepareInfo& info) override {
        sampleRate_ = info.sampleRate;
        buffer_.assign(static_cast<std::size_t>(info.blockSize), 0.0f);
    }

    void adoptState(primitive& outgoing) noexcept override {
        auto& o = static_cast<lfo&>(outgoing);
        position_ = o.position_;
        held_ = o.held_;
        random_ = o.random_;
    }

    void process(const blockContext& ctx) noexcept override {
        const int n = ctx.numSamples;
        const auto rate = rate_.block(0, n);
        const auto phase = phase_.block(0, n);
        const auto depth = depth_.block(0, n);
        const auto offset = offset_.block(0, n);
        for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) {
            double t = position_ + phase[i];
            t -= std::floor(t);
            buffer_[i] = static_cast<float>(shape(t)) * depth[i] + offset[i];
            position_ += rate[i] / sampleRate_;
            if (position_ >= 1.0) {
                position_ -= std::floor(position_);
                held_ = random_.bipolar();
            }
        }
        writeOut(out_, std::span(buffer_).first(static_cast<std::size_t>(n)));
    }

private:
    double shape(double t) const noexcept {
        switch (shape_) {
        case 0: return std::sin(twoPi * t);
        case 1: return t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t;
        case 2: return 2.0 * t - 1.0;
        case 3: return t < 0.5 ? 1.0 : -1.0;
        default: return held_;
        }
    }

    int shape_;
    signal rate_;
    signal phase_;
    signal depth_;
    signal offset_;
    signalOut out_;
    rng random_;
    double sampleRate_ = 48000.0;
    double position_ = 0.0;
    float held_ = 0.0f;
    std::vector<float> buffer_;
};

// --- smoother -----------------------------------------------------------

class smoother final : public modulator {
public:
    explicit smoother(nodeContext& c)
        : modulator(c.id(), c.type()),
          time_(c.number("time")),
          linear_(c.choice("curve") == 1),
          in_(c.signalIn("in")),
          out_(c.signalOutput("out")) {}

    void prepare(const prepareInfo& info) override {
        const double samples = std::max(time_ * info.sampleRate, 1.0);
        coefficient_ = std::exp(-1.0 / samples);
        linearSamples_ = samples;
        buffer_.assign(static_cast<std::size_t>(info.blockSize), 0.0f);
    }

    void adoptState(primitive& outgoing) noexcept override {
        auto& o = static_cast<smoother&>(outgoing);
        state_ = o.state_;
        primed_ = o.primed_;
    }

    void process(const blockContext& ctx) noexcept override {
        const auto n = static_cast<std::size_t>(ctx.numSamples);
        for (int ch = 0; ch < out_.channels; ++ch) {
            const auto x = in_.block(ch, ctx.numSamples);
            auto& [y, step, target] = state_[static_cast<std::size_t>(ch)];
            if (!primed_ && !x.empty()) y = target = x[0];
            for (std::size_t i = 0; i < n; ++i) {
                if (linear_) {
                    if (x[i] != target) {
                        target = x[i];
                        step = (target - y) / linearSamples_;
                    }
                    y = std::abs(target - y) <= std::abs(step) ? target : y + step;
                } else {
                    y = x[i] + coefficient_ * (y - x[i]);
                }
                buffer_[i] = static_cast<float>(y);
            }
            if (out_.audio) {
                emit(*out_.audio, ch, std::span(buffer_).first(n));
            } else if (n > 0) {
                emit(*out_.control, ch, buffer_[n - 1]);
            }
        }
        primed_ = true;
    }

private:
    struct channelState {
        double y = 0.0;
        double step = 0.0;
        double target = 0.0;
    };

    double time_;
    bool linear_;
    signal in_;
    signalOut out_;
    double coefficient_ = 0.0;
    double linearSamples_ = 1.0;
    bool primed_ = false;
    std::array<channelState, maxChannels> state_{};
    std::vector<float> buffer_;
};

const factoryEntry factories[] = {
    {"adsr", make<adsr>},
    {"lfo", make<lfo>},
    {"smoother", make<smoother>},
};

} // namespace

std::span<const factoryEntry> modulationFactories() { return factories; }

} // namespace primitives
