// Filters: biquad, state-variable, and ladder (primitives.md §7.6.1,
// §7.6.2). Each channel of the output has its own state; a mono input
// feeds every channel.

#include "node.h"

#include <cmath>

namespace primitives {

namespace {

// Base for filters with one audio input and per-channel state.
class channelFilter : public primitive {
public:
    explicit channelFilter(nodeContext& c, std::string_view outPort = "out")
        : primitive(c.id(), c.type()), in_(c.signalIn("in")), channels_(c.outputChannels(outPort)) {}

    void prepare(const prepareInfo& info) override {
        sampleRate_ = info.sampleRate;
        buffer_.assign(static_cast<std::size_t>(info.blockSize), 0.0f);
    }

protected:
    signal in_;
    int channels_;
    double sampleRate_ = 48000.0;
    std::vector<float> buffer_;
};

// --- biquad (RBJ Audio EQ Cookbook) --------------------------------------

class biquad final : public channelFilter {
public:
    explicit biquad(nodeContext& c)
        : channelFilter(c),
          mode_(c.choice("mode")),
          freq_(c.param("freq")),
          q_(c.param("q")),
          gain_(c.param("gain")),
          out_(c.audioOut("out")) {}

    void adoptState(primitive& outgoing) noexcept override {
        state_ = static_cast<biquad&>(outgoing).state_;
    }

    void process(const blockContext& ctx) noexcept override {
        computeCoefficients();
        const auto n = static_cast<std::size_t>(ctx.numSamples);
        for (int ch = 0; ch < channels_; ++ch) {
            const auto x = in_.block(ch, ctx.numSamples);
            auto& [z1, z2] = state_[static_cast<std::size_t>(ch)];
            for (std::size_t i = 0; i < n; ++i) {
                const double y = b0_ * x[i] + z1;
                z1 = b1_ * x[i] - a1_ * y + z2;
                z2 = b2_ * x[i] - a2_ * y;
                buffer_[i] = static_cast<float>(y);
            }
            emit(*out_, ch, {buffer_.data(), n});
        }
    }

private:
    void computeCoefficients() noexcept {
        const double f = std::clamp(static_cast<double>(freq_.value()), 1.0, sampleRate_ * 0.49);
        const double q = std::max(static_cast<double>(q_.value()), 0.01);
        const double a = std::sqrt(std::max(static_cast<double>(gain_.value()), 1e-6));  // 10^(dB/40)
        const double w = twoPi * f / sampleRate_;
        const double cw = std::cos(w);
        const double alpha = std::sin(w) / (2.0 * q);
        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
        switch (mode_) {
        case 0:  // low-pass
            b0 = (1 - cw) / 2, b1 = 1 - cw, b2 = (1 - cw) / 2, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
            break;
        case 1:  // high-pass
            b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = (1 + cw) / 2, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
            break;
        case 2:  // band-pass, 0 dB peak
            b0 = alpha, b1 = 0, b2 = -alpha, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
            break;
        case 3:  // notch
            b0 = 1, b1 = -2 * cw, b2 = 1, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
            break;
        case 4:  // all-pass
            b0 = 1 - alpha, b1 = -2 * cw, b2 = 1 + alpha, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
            break;
        case 5:  // peaking
            b0 = 1 + alpha * a, b1 = -2 * cw, b2 = 1 - alpha * a;
            a0 = 1 + alpha / a, a1 = -2 * cw, a2 = 1 - alpha / a;
            break;
        case 6: {  // low shelf
            const double s = 2 * std::sqrt(a) * alpha;
            b0 = a * ((a + 1) - (a - 1) * cw + s), b1 = 2 * a * ((a - 1) - (a + 1) * cw);
            b2 = a * ((a + 1) - (a - 1) * cw - s), a0 = (a + 1) + (a - 1) * cw + s;
            a1 = -2 * ((a - 1) + (a + 1) * cw), a2 = (a + 1) + (a - 1) * cw - s;
            break;
        }
        default: {  // high shelf
            const double s = 2 * std::sqrt(a) * alpha;
            b0 = a * ((a + 1) + (a - 1) * cw + s), b1 = -2 * a * ((a - 1) + (a + 1) * cw);
            b2 = a * ((a + 1) + (a - 1) * cw - s), a0 = (a + 1) - (a - 1) * cw + s;
            a1 = 2 * ((a - 1) - (a + 1) * cw), a2 = (a + 1) - (a - 1) * cw - s;
        }
        }
        b0_ = b0 / a0, b1_ = b1 / a0, b2_ = b2 / a0, a1_ = a1 / a0, a2_ = a2 / a0;
    }

    int mode_;
    signal freq_;
    signal q_;
    signal gain_;
    aPipe* out_;
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    std::array<std::pair<double, double>, maxChannels> state_{};
};

// --- svf (Simper's trapezoidal state-variable filter) -------------------

class svf final : public channelFilter {
public:
    explicit svf(nodeContext& c)
        : channelFilter(c, c.hasOutput("out") ? "out" : firstOutput(c)),
          mode_(c.choice("mode")),
          cutoff_(c.param("cutoff")),
          q_(c.param("q")),
          outs_{c.audioOut("out"), c.audioOut("low"), c.audioOut("band"), c.audioOut("high"), c.audioOut("notch")} {}

    void prepare(const prepareInfo& info) override {
        channelFilter::prepare(info);
        for (auto& b : taps_) b.assign(static_cast<std::size_t>(info.blockSize), 0.0f);
    }

    void adoptState(primitive& outgoing) noexcept override { state_ = static_cast<svf&>(outgoing).state_; }

    void process(const blockContext& ctx) noexcept override {
        const auto n = static_cast<std::size_t>(ctx.numSamples);
        for (int ch = 0; ch < channels_; ++ch) {
            const auto x = in_.block(ch, ctx.numSamples);
            const auto fc = cutoff_.block(ch, ctx.numSamples);
            const auto q = q_.block(ch, ctx.numSamples);
            auto& [ic1, ic2] = state_[static_cast<std::size_t>(ch)];
            for (std::size_t i = 0; i < n; ++i) {
                const double g = std::tan(pi * std::clamp(static_cast<double>(fc[i]), 1.0, sampleRate_ * 0.49) / sampleRate_);
                const double k = 1.0 / std::max(static_cast<double>(q[i]), 0.01);
                const double a1 = 1.0 / (1.0 + g * (g + k));
                const double a2 = g * a1;
                const double a3 = g * a2;
                const double v3 = x[i] - ic2;
                const double v1 = a1 * ic1 + a2 * v3;
                const double v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2.0 * v1 - ic1;
                ic2 = 2.0 * v2 - ic2;
                const double high = x[i] - k * v1 - v2;
                taps_[0][i] = static_cast<float>(v2);          // low
                taps_[1][i] = static_cast<float>(v1);          // band
                taps_[2][i] = static_cast<float>(high);        // high
                taps_[3][i] = static_cast<float>(v2 + high);   // notch
            }
            if (outs_[0]) emit(*outs_[0], ch, {taps_[static_cast<std::size_t>(mode_)].data(), n});
            for (std::size_t t = 0; t < taps_.size(); ++t) {
                if (outs_[t + 1]) emit(*outs_[t + 1], ch, {taps_[t].data(), n});
            }
        }
    }

private:
    static std::string_view firstOutput(nodeContext& c) {
        for (std::string_view p : {"low", "band", "high", "notch"}) {
            if (c.hasOutput(p)) return p;
        }
        c.fail("svf needs at least one output");
    }

    int mode_;
    signal cutoff_;
    signal q_;
    std::array<aPipe*, 5> outs_;
    std::array<std::vector<float>, 4> taps_;
    std::array<std::pair<double, double>, maxChannels> state_{};
};

// --- ladder (zero-delay-feedback 4-pole, after Zavalishin) ---------------

class ladder final : public channelFilter {
public:
    explicit ladder(nodeContext& c)
        : channelFilter(c),
          cutoff_(c.param("cutoff")),
          resonance_(c.param("resonance")),
          drive_(c.param("drive")),
          out_(c.audioOut("out")) {}

    void adoptState(primitive& outgoing) noexcept override { state_ = static_cast<ladder&>(outgoing).state_; }

    void process(const blockContext& ctx) noexcept override {
        const auto n = static_cast<std::size_t>(ctx.numSamples);
        for (int ch = 0; ch < channels_; ++ch) {
            const auto x = in_.block(ch, ctx.numSamples);
            const auto fc = cutoff_.block(ch, ctx.numSamples);
            const auto res = resonance_.block(ch, ctx.numSamples);
            const auto drive = drive_.block(ch, ctx.numSamples);
            auto& s = state_[static_cast<std::size_t>(ch)];
            for (std::size_t i = 0; i < n; ++i) {
                const double g = std::tan(pi * std::clamp(static_cast<double>(fc[i]), 1.0, sampleRate_ * 0.45) / sampleRate_);
                const double G = g / (1.0 + g);
                // Feedback gain 4 is the linear oscillation threshold; a little
                // more keeps the saturated loop oscillating at resonance 1.
                const double k = 4.2 * std::clamp(static_cast<double>(res[i]), 0.0, 1.0);
                const double sigma = (G * G * G * s[0] + G * G * s[1] + G * s[2] + s[3]) / (1.0 + g);
                const double input = std::tanh(static_cast<double>(drive[i]) * x[i]);
                double u = std::tanh((input - k * sigma) / (1.0 + k * G * G * G * G));
                for (auto& stage : s) {
                    const double v = (u - stage) * G;
                    const double y = v + stage;
                    stage = y + v;
                    u = y;
                }
                buffer_[i] = static_cast<float>(u);
            }
            emit(*out_, ch, {buffer_.data(), n});
        }
    }

private:
    signal cutoff_;
    signal resonance_;
    signal drive_;
    aPipe* out_;
    std::array<std::array<double, 4>, maxChannels> state_{};
};

constexpr factoryEntry factories[] = {
    {"biquad", make<biquad>},
    {"svf", make<svf>},
    {"ladder", make<ladder>},
};

} // namespace

std::span<const factoryEntry> filterFactories() { return factories; }

} // namespace primitives
