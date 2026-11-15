// Filters: biquad, state-variable, and ladder (primitives.md §7.6.1,
// §7.6.2). Each channel of the output has its own state; a mono input
// feeds every channel.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>

#include <proj/primitives/primitive.h>
#include <proj/primitives/realtime.h>

#include "node.h"

namespace primitives {

namespace {

// Base for filters with one audio input and per-channel state.
class channelFilter : public nodeBase {
public:
    explicit channelFilter(nodeContext& c)
        : nodeBase(c), in_(c.signalIn("in")), sampleRate_(c.sampleRate()), buffer_(c.blockSize()) {}

protected:
    signal in_;
    double sampleRate_ = 48000.0;
    blockBuffer buffer_;
};

// --- biquad (RBJ Audio EQ Cookbook) --------------------------------------

// Every parameter can be an audio pipe (§7.6.3), so each channel reads its
// own frequency, Q, and gain, and the coefficients follow them every
// controlInterval samples (3 kHz at 48 kHz): a direct-form biquad needs a
// cosine, a sine, and a square root per update, too many per sample.
class biquad final : public channelFilter {
public:
    static constexpr std::size_t controlInterval = 16;

    explicit biquad(nodeContext& c)
        : channelFilter(c),
          mode_(c.choice("mode")),
          freq_(c.param("freq")),
          q_(c.param("q")),
          gain_(c.param("gain")),
          out_(c.audioOut("out")) {}

    void adoptState(primitive& outgoing) noexcept PROJ_NONBLOCKING override {
        adoptFinite(state_, static_cast<const biquad&>(outgoing).state_);
    }

    void process(const blockContext& ctx) noexcept PROJ_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto count = static_cast<std::size_t>(n);
        for (int ch = 0; ch < out_.channels; ++ch) {
            const auto x = in_.block(ch, n);
            const auto f = freq_.block(ch, n);
            const auto q = q_.block(ch, n);
            const auto g = gain_.block(ch, n);
            auto& [z1, z2] = state_[static_cast<std::size_t>(ch)];
            for (std::size_t i = 0; i < count; ++i) {
                if (i % controlInterval == 0) update(f[i], q[i], g[i]);
                const double y = b0_ * x[i] + z1;
                z1 = b1_ * x[i] - a1_ * y + z2;
                z2 = b2_ * x[i] - a2_ * y;
                buffer_[i] = static_cast<float>(y);
            }
            emit(out_, ch, buffer_.first(count));
        }
    }

private:
    // Recomputes the coefficients when a parameter changed.
    void update(float freq, float q, float gain) noexcept {
        if (freq == lastFreq_ && q == lastQ_ && gain == lastGain_) return;
        lastFreq_ = freq;
        lastQ_ = q;
        lastGain_ = gain;
        computeCoefficients(freq, q, gain);
    }

    void computeCoefficients(float freq, float quality, float gain) noexcept {
        const double f = std::clamp(static_cast<double>(freq), 1.0, sampleRate_ * 0.49);
        const double q = std::max(static_cast<double>(quality), 0.01);
        const double a = std::sqrt(std::max(static_cast<double>(gain), 1e-6));  // 10^(dB/40)
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

    int mode_ = 0;
    signal freq_;
    signal q_;
    signal gain_;
    audioOutput out_;
    // The parameters the coefficients were computed from; NaN before the first.
    float lastFreq_ = notANumber;
    float lastQ_ = notANumber;
    float lastGain_ = notANumber;
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    std::array<std::pair<double, double>, maxChannels> state_{};
};

// --- svf (Simper's trapezoidal state-variable filter) -------------------

class svf final : public channelFilter {
public:
    explicit svf(nodeContext& c)
        : channelFilter(c),
          mode_(c.choice("mode")),
          cutoff_(c.param("cutoff")),
          qParam_(c.param("q")),
          outs_{c.audioOut("out"), c.audioOut("low"), c.audioOut("band"), c.audioOut("high"),
                c.audioOut("notch")},
          taps_{blockBuffer(c.blockSize()), blockBuffer(c.blockSize()), blockBuffer(c.blockSize()),
                blockBuffer(c.blockSize())} {
        // Each output has its own channel count; the filter runs as many
        // channels as the widest.
        for (const auto& o : outs_) {
            if (o.pipe) channels_ = std::max(channels_, o.channels);
        }
        if (channels_ == 0) c.fail("svf needs at least one output");
    }

    void adoptState(primitive& outgoing) noexcept PROJ_NONBLOCKING override {
        adoptFinite(state_, static_cast<const svf&>(outgoing).state_);
    }

    void process(const blockContext& ctx) noexcept PROJ_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto count = static_cast<std::size_t>(n);
        for (int ch = 0; ch < channels_; ++ch) {
            const auto x = in_.block(ch, n);
            const auto fc = cutoff_.block(ch, n);
            const auto q = qParam_.block(ch, n);
            auto& [ic1, ic2] = state_[static_cast<std::size_t>(ch)];
            for (std::size_t i = 0; i < count; ++i) {
                if (fc[i] != fc_ || q[i] != q_) updateCoefficients(fc[i], q[i]);
                const double v3 = x[i] - ic2;
                const double v1 = a1_ * ic1 + a2_ * v3;
                const double v2 = ic2 + a2_ * ic1 + a3_ * v3;
                ic1 = 2.0 * v1 - ic1;
                ic2 = 2.0 * v2 - ic2;
                const double high = x[i] - k_ * v1 - v2;
                taps_[0][i] = static_cast<float>(v2);         // low
                taps_[1][i] = static_cast<float>(v1);         // band
                taps_[2][i] = static_cast<float>(high);       // high
                taps_[3][i] = static_cast<float>(v2 + high);  // notch
            }
            emit(outs_[0], ch, taps_[static_cast<std::size_t>(mode_)].first(count));
            for (std::size_t t = 0; t < taps_.size(); ++t) emit(outs_[t + 1], ch, taps_[t].first(count));
        }
    }

private:
    // tan() per sample only when the cutoff or Q changes.
    void updateCoefficients(float fc, float q) noexcept {
        fc_ = fc;
        q_ = q;
        const double g = std::tan(pi * std::clamp(static_cast<double>(fc), 1.0, sampleRate_ * 0.49) / sampleRate_);
        k_ = 1.0 / std::max(static_cast<double>(q), 0.01);
        a1_ = 1.0 / (1.0 + g * (g + k_));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }

    int mode_ = 0;
    signal cutoff_;
    signal qParam_;
    // out (the selected mode), then low, band, high, and notch.
    std::array<audioOutput, 5> outs_;
    int channels_ = 0;
    float fc_ = notANumber;
    float q_ = notANumber;
    double k_ = 0.0, a1_ = 0.0, a2_ = 0.0, a3_ = 0.0;
    std::array<blockBuffer, 4> taps_;
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

    void adoptState(primitive& outgoing) noexcept PROJ_NONBLOCKING override {
        adoptFinite(state_, static_cast<const ladder&>(outgoing).state_);
    }

    void process(const blockContext& ctx) noexcept PROJ_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto count = static_cast<std::size_t>(n);
        for (int ch = 0; ch < out_.channels; ++ch) {
            const auto x = in_.block(ch, n);
            const auto fc = cutoff_.block(ch, n);
            const auto res = resonance_.block(ch, n);
            const auto drive = drive_.block(ch, n);
            auto& s = state_[static_cast<std::size_t>(ch)];
            for (std::size_t i = 0; i < count; ++i) {
                // Coefficients change only with the cutoff or resonance.
                if (fc[i] != fc_ || res[i] != res_) updateCoefficients(fc[i], res[i]);
                const double sigma = (G3_ * s[0] + G2_ * s[1] + G_ * s[2] + s[3]) * invOnePlusG_;
                const double input = std::tanh(static_cast<double>(drive[i]) * x[i]);
                double u = std::tanh((input - k_ * sigma) * invDenominator_);
                const double G = G_;
                for (auto& stage : s) {
                    const double v = (u - stage) * G;
                    const double y = v + stage;
                    stage = y + v;
                    u = y;
                }
                buffer_[i] = static_cast<float>(u);
            }
            emit(out_, ch, buffer_.first(count));
        }
    }

private:
    void updateCoefficients(float fc, float res) noexcept {
        fc_ = fc;
        res_ = res;
        const double g = std::tan(pi * std::clamp(static_cast<double>(fc), 1.0, sampleRate_ * 0.45) / sampleRate_);
        G_ = g / (1.0 + g);
        G2_ = G_ * G_;
        G3_ = G2_ * G_;
        invOnePlusG_ = 1.0 / (1.0 + g);
        // Feedback gain 4 is the linear oscillation threshold; a little
        // more keeps the saturated loop oscillating at resonance 1.
        k_ = 4.2 * std::clamp(static_cast<double>(res), 0.0, 1.0);
        invDenominator_ = 1.0 / (1.0 + k_ * G3_ * G_);
    }

    signal cutoff_;
    signal resonance_;
    signal drive_;
    audioOutput out_;
    float fc_ = notANumber;
    float res_ = notANumber;
    double G_ = 0.0, G2_ = 0.0, G3_ = 0.0, invOnePlusG_ = 1.0, k_ = 0.0, invDenominator_ = 1.0;
    std::array<std::array<double, 4>, maxChannels> state_{};
};

} // namespace

std::span<const factoryEntry> filterFactories() {
    static const auto factories = std::to_array<factoryEntry>({
        {.type = "biquad", .make = make<biquad>},
        {.type = "svf", .make = make<svf>},
        {.type = "ladder", .make = make<ladder>},
    });
    return factories;
}

} // namespace primitives
