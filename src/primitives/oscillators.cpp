// Oscillators and noise (primitives.md §7.6). Generators without audio
// inputs produce one signal and write it to every channel of their output.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

#include <espresso/primitives/primitive.h>
#include <espresso/primitives/realtime.h>

#include "node.h"

namespace primitives {

namespace {

// PolyBLEP residual for a discontinuity at phase 0 (Välimäki and
// Huovilainen): subtracting it from a naive step band-limits the step.
inline float polyBlep(double t, double dt) noexcept {
    if (t < dt) {
        t /= dt;
        return static_cast<float>(t + t - t * t - 1.0);
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return static_cast<float>(t * t + t + t + 1.0);
    }
    return 0.0f;
}

inline double wrap(double p) noexcept { return p - std::floor(p); }

// Shapes without state.
struct stateless {
    bool isFinite() const noexcept { return true; }
};

struct sineShape : stateless {
    float operator()(double t, double, float) noexcept { return static_cast<float>(std::sin(twoPi * t)); }
};

struct sawShape : stateless {
    float operator()(double t, double dt, float) noexcept {
        return static_cast<float>(2.0 * t - 1.0) - polyBlep(t, dt);
    }
};

struct pulseShape : stateless {
    float operator()(double t, double dt, float width) noexcept {
        const double w = std::clamp(static_cast<double>(width), 0.001, 0.999);
        const float naive = t < w ? 1.0f : -1.0f;
        return naive + polyBlep(t, dt) - polyBlep(wrap(t + 1.0 - w), dt);
    }
};

// A band-limited square, integrated by a leaky integrator.
struct triangleShape {
    // The integrator, from the naive triangle at the first sample.
    std::optional<float> state;

    bool isFinite() const noexcept { return !state || std::isfinite(*state); }

    float operator()(double t, double dt, float) noexcept {
        if (!state) state = static_cast<float>(t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t);
        const float square = pulseShape{}(t, dt, 0.5f);
        // The leak removes drift from rounding without audibly bending the ramps.
        *state = *state * static_cast<float>(1.0 - 0.01 * dt) + static_cast<float>(4.0 * dt) * square;
        return *state;
    }
};

template <typename Shape>
class oscillator final : public nodeBase {
public:
    explicit oscillator(nodeContext& c)
        : nodeBase(c),
          freq_(c.param("freq")),
          phase_(c.param("phase")),
          width_(c.paramOr("width", 0.5f)),
          out_(c.audioOut("out")),
          sampleRate_(c.sampleRate()),
          buffer_(c.blockSize()) {}

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        const auto& o = static_cast<const oscillator&>(outgoing);
        if (!allFinite(o.position_, o.shape_)) return;
        position_ = o.position_;
        shape_ = o.shape_;
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto count = static_cast<std::size_t>(n);
        const auto f = freq_.block(0, n);
        const auto ph = phase_.block(0, n);
        const auto w = width_.block(0, n);
        for (std::size_t i = 0; i < count; ++i) {
            const double dt = std::min(std::abs(static_cast<double>(f[i])) / sampleRate_, 0.5);
            buffer_[i] = shape_(wrap(position_ + ph[i]), dt, w[i]);
            position_ = wrap(position_ + f[i] / sampleRate_);
        }
        emitAll(out_, buffer_.first(count));
    }

private:
    signal freq_;
    signal phase_;
    signal width_;
    audioOutput out_;
    double sampleRate_ = 48000.0;
    double position_ = 0.0;
    Shape shape_{};
    blockBuffer buffer_;
};

class noise final : public nodeBase {
public:
    explicit noise(nodeContext& c)
        : nodeBase(c),
          color_(c.choice("color")),
          out_(c.audioOut("out")),
          random_(c.seed()),
          buffer_(c.blockSize()) {}

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        const auto& o = static_cast<const noise&>(outgoing);
        random_ = o.random_;
        pink_ = o.pink_;
        brown_ = o.brown_;
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const auto n = static_cast<std::size_t>(ctx.numSamples);
        for (std::size_t i = 0; i < n; ++i) {
            const float white = random_.bipolar();
            switch (color_) {
            case 0: buffer_[i] = white; break;
            case 1: buffer_[i] = pink(white); break;
            default:
                brown_ = (brown_ + 0.02f * white) / 1.02f;
                buffer_[i] = brown_ * 3.5f;
            }
        }
        emitAll(out_, buffer_.first(n));
    }

private:
    // Paul Kellet's refined pink noise filter.
    float pink(float white) noexcept {
        auto& b = pink_;
        b[0] = 0.99886f * b[0] + white * 0.0555179f;
        b[1] = 0.99332f * b[1] + white * 0.0750759f;
        b[2] = 0.96900f * b[2] + white * 0.1538520f;
        b[3] = 0.86650f * b[3] + white * 0.3104856f;
        b[4] = 0.55000f * b[4] + white * 0.5329522f;
        b[5] = -0.7616f * b[5] - white * 0.0168980f;
        const float out = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + white * 0.5362f;
        b[6] = white * 0.115926f;
        return out * 0.11f;
    }

    int color_ = 0;
    audioOutput out_;
    rng random_;
    std::array<float, 7> pink_{};
    float brown_ = 0.0f;
    blockBuffer buffer_;
};

} // namespace

std::span<const factoryEntry> oscillatorFactories() {
    static const auto factories = std::to_array<factoryEntry>({
        {.type = "sineOsc", .make = make<oscillator<sineShape>>},
        {.type = "sawOsc", .make = make<oscillator<sawShape>>},
        {.type = "pulseOsc", .make = make<oscillator<pulseShape>>},
        {.type = "triangleOsc", .make = make<oscillator<triangleShape>>},
        {.type = "noise", .make = make<noise>},
    });
    return factories;
}

} // namespace primitives
