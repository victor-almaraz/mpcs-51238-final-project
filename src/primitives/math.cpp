// Math, routing, conversion, and mapper operations (primitives.md §7.6,
// §7.9). Each works on audio pipes sample by sample and on control pipes
// value by value, matching its output.

#include "node.h"

#include <cmath>

namespace primitives {

namespace {

// --- Element-wise operations ---------------------------------------------

// The operation's parameters, in the order its spec names them.
using opParams = std::array<float, 4>;
// Parameter names; empty names are unused.
using opNames = std::array<std::string_view, 4>;

// Each operation is a stateless type: elementwise<Op> calls Op::apply
// directly, so the per-sample loop has no indirect call.
struct gainOp {
    static constexpr opNames params{"amount"};
    static float apply(float x, const opParams& p, double) noexcept { return x * p[0]; }
};
struct clipOp {
    static constexpr opNames params{"low", "high"};
    static float apply(float x, const opParams& p, double) noexcept {
        return std::clamp(x, std::min(p[0], p[1]), std::max(p[0], p[1]));
    }
};
struct dbToAmpOp {
    static constexpr opNames params{};
    static float apply(float x, const opParams&, double) noexcept { return std::pow(10.0f, x / 20.0f); }
};
struct beatsToSecondsOp {
    static constexpr opNames params{};
    static float apply(float x, const opParams&, double tempo) noexcept {
        return static_cast<float>(x * 60.0 / tempo);
    }
};
struct negOp {
    static constexpr opNames params{};
    static float apply(float x, const opParams&, double) noexcept { return -x; }
};
struct absOp {
    static constexpr opNames params{};
    static float apply(float x, const opParams&, double) noexcept { return std::abs(x); }
};
struct expOp {
    static constexpr opNames params{};
    static float apply(float x, const opParams&, double) noexcept { return std::exp(x); }
};
struct logOp {
    static constexpr opNames params{};
    static float apply(float x, const opParams&, double) noexcept { return x > 0.0f ? std::log(x) : -1e30f; }
};
struct sinOp {
    static constexpr opNames params{};
    static float apply(float x, const opParams&, double) noexcept { return std::sin(x); }
};
struct cosOp {
    static constexpr opNames params{};
    static float apply(float x, const opParams&, double) noexcept { return std::cos(x); }
};
struct tanhOp {
    static constexpr opNames params{};
    static float apply(float x, const opParams&, double) noexcept { return std::tanh(x); }
};
struct floorOp {
    static constexpr opNames params{};
    static float apply(float x, const opParams&, double) noexcept { return std::floor(x); }
};
struct roundOp {
    static constexpr opNames params{};
    static float apply(float x, const opParams&, double) noexcept { return std::round(x); }
};
struct subtractOp {
    static constexpr opNames params{"b"};
    static float apply(float x, const opParams& p, double) noexcept { return x - p[0]; }
};
struct divideOp {
    static constexpr opNames params{"b"};
    static float apply(float x, const opParams& p, double) noexcept { return p[0] == 0.0f ? 0.0f : x / p[0]; }
};
struct powOp {
    static constexpr opNames params{"b"};
    static float apply(float x, const opParams& p, double) noexcept { return std::pow(x, p[0]); }
};
struct minOp {
    static constexpr opNames params{"b"};
    static float apply(float x, const opParams& p, double) noexcept { return std::min(x, p[0]); }
};
struct maxOp {
    static constexpr opNames params{"b"};
    static float apply(float x, const opParams& p, double) noexcept { return std::max(x, p[0]); }
};
struct quantizeOp {
    static constexpr opNames params{"step"};
    static float apply(float x, const opParams& p, double) noexcept {
        return p[0] > 0.0f ? std::round(x / p[0]) * p[0] : x;
    }
};
struct scaleOp {
    static constexpr opNames params{"in-low", "in-high", "out-low", "out-high"};
    static float apply(float x, const opParams& p, double) noexcept {
        const float span = p[1] - p[0];
        const float t = span == 0.0f ? 0.0f : (x - p[0]) / span;
        return p[2] + t * (p[3] - p[2]);
    }
};
struct scaleExpOp {
    static constexpr opNames params{"in-low", "in-high", "out-low", "out-high"};
    static float apply(float x, const opParams& p, double) noexcept {
        const float span = p[1] - p[0];
        const float t = span == 0.0f ? 0.0f : (x - p[0]) / span;
        // Output bounds of the same sign (§7.9.1); otherwise linear.
        if (p[2] == 0.0f || p[3] == 0.0f || (p[2] < 0.0f) != (p[3] < 0.0f)) return p[2] + t * (p[3] - p[2]);
        return p[2] * std::pow(p[3] / p[2], t);
    }
};

// Parameters an operation names.
constexpr std::size_t paramCount(const opNames& names) noexcept {
    return static_cast<std::size_t>(std::find(names.begin(), names.end(), std::string_view{}) -
                                    names.begin());
}

template <typename Op>
class elementwise final : public nodeBase {
public:
    static constexpr std::size_t count = paramCount(Op::params);

    explicit elementwise(nodeContext& c)
        : nodeBase(c), in_(c.signalIn("in")), out_(c.signalOutput("out")), buffer_(c.blockSize()) {
        for (std::size_t k = 0; k < count; ++k) params_[k] = c.param(Op::params[k]);
    }

    void process(const blockContext& ctx) noexcept PROJ_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto samples = static_cast<std::size_t>(n);
        opParams p{};
        for (int ch = 0; ch < out_.channels; ++ch) {
            if (out_.control) {
                for (std::size_t k = 0; k < count; ++k) p[k] = params_[k].value(ch);
                emit(*out_.control, ch, Op::apply(in_.value(ch), p, ctx.tempo));
                continue;
            }
            const auto x = in_.block(ch, n);
            std::array<std::span<const float>, 4> blocks;
            for (std::size_t k = 0; k < count; ++k) blocks[k] = params_[k].block(ch, n);
            for (std::size_t i = 0; i < samples; ++i) {
                for (std::size_t k = 0; k < count; ++k) p[k] = blocks[k][i];
                buffer_[i] = Op::apply(x[i], p, ctx.tempo);
            }
            emit(out_, ch, buffer_.first(samples));
        }
    }

private:
    signal in_;
    signalOut out_;
    std::array<signal, count> params_;
    blockBuffer buffer_;
};

// --- Reductions over several inputs: add, multiply, mix -----------------

enum class reduction { sum, product, mix };

template <reduction R>
class reduce final : public nodeBase {
public:
    explicit reduce(nodeContext& c) : nodeBase(c), out_(c.signalOutput("out")), buffer_(c.blockSize()) {
        for (std::size_t k = 0; k < c.inputCount("in"); ++k) inputs_.push_back(c.signalIn("in", k));
        if constexpr (R == reduction::mix) {
            const auto levels = c.list("levels");
            if (!levels.empty() && levels.size() != inputs_.size()) c.fail("levels must have one entry per input");
            for (std::size_t k = 0; k < inputs_.size(); ++k) {
                levels_.push_back(levels.empty() ? 1.0f : static_cast<float>(levels[k]));
            }
        } else {
            levels_.assign(inputs_.size(), 1.0f);
        }
    }

    void process(const blockContext& ctx) noexcept PROJ_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto samples = static_cast<std::size_t>(n);
        constexpr float identity = R == reduction::product ? 1.0f : 0.0f;
        for (int ch = 0; ch < out_.channels; ++ch) {
            if (out_.control) {
                float acc = identity;
                for (std::size_t k = 0; k < inputs_.size(); ++k) combine(acc, inputs_[k].value(ch), levels_[k]);
                emit(*out_.control, ch, acc);
                continue;
            }
            const auto acc = buffer_.first(samples);
            std::fill(acc.begin(), acc.end(), identity);
            for (std::size_t k = 0; k < inputs_.size(); ++k) {
                const auto x = inputs_[k].block(ch, n);
                for (std::size_t i = 0; i < samples; ++i) combine(acc[i], x[i], levels_[k]);
            }
            emit(out_, ch, acc);
        }
    }

private:
    static void combine(float& acc, float x, float level) noexcept {
        if constexpr (R == reduction::product) {
            acc *= x;
        } else {
            acc += x * level;
        }
    }

    std::vector<signal> inputs_;
    std::vector<float> levels_;
    signalOut out_;
    blockBuffer buffer_;
};

// --- pan ----------------------------------------------------------------

class pan final : public nodeBase {
public:
    explicit pan(nodeContext& c)
        : nodeBase(c),
          in_(c.signalIn("in")),
          position_(c.param("position")),
          out_(c.audioOut("out")),
          left_(c.blockSize()),
          right_(c.blockSize()) {
        if (out_.channels < 2) c.fail("pan needs an output with two channels");
    }

    void process(const blockContext& ctx) noexcept PROJ_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto count = static_cast<std::size_t>(n);
        const auto x = in_.block(0, n);
        const auto pos = position_.block(0, n);
        for (std::size_t i = 0; i < count; ++i) {
            const double angle = (std::clamp(pos[i], -1.0f, 1.0f) + 1.0) * pi / 4.0;  // equal power
            left_[i] = static_cast<float>(x[i] * std::cos(angle));
            right_[i] = static_cast<float>(x[i] * std::sin(angle));
        }
        emit(out_, 0, left_.first(count));
        emit(out_, 1, right_.first(count));
    }

private:
    signal in_;
    signal position_;
    audioOutput out_;
    blockBuffer left_;
    blockBuffer right_;
};

// --- crossfade ----------------------------------------------------------

class crossfade final : public nodeBase {
public:
    explicit crossfade(nodeContext& c)
        : nodeBase(c),
          a_(c.signalIn("a")),
          b_(c.signalIn("b")),
          position_(c.param("position")),
          linear_(c.choice("law") == 1),
          out_(c.signalOutput("out")),
          buffer_(c.blockSize()) {}

    void process(const blockContext& ctx) noexcept PROJ_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto count = static_cast<std::size_t>(n);
        for (int ch = 0; ch < out_.channels; ++ch) {
            if (out_.control) {
                emit(*out_.control, ch, mixAt(a_.value(ch), b_.value(ch), position_.value(ch)));
                continue;
            }
            const auto a = a_.block(ch, n);
            const auto b = b_.block(ch, n);
            const auto pos = position_.block(ch, n);
            for (std::size_t i = 0; i < count; ++i) buffer_[i] = mixAt(a[i], b[i], pos[i]);
            emit(out_, ch, buffer_.first(count));
        }
    }

private:
    float mixAt(float a, float b, float position) const noexcept {
        const double t = std::clamp(static_cast<double>(position), 0.0, 1.0);
        if (linear_) return static_cast<float>(a * (1.0 - t) + b * t);
        return static_cast<float>(a * std::cos(t * pi / 2.0) + b * std::sin(t * pi / 2.0));
    }

    signal a_;
    signal b_;
    signal position_;
    bool linear_ = false;
    signalOut out_;
    blockBuffer buffer_;
};

} // namespace

std::span<const factoryEntry> mathFactories() {
    static const factoryEntry factories[] = {
        {"add", make<reduce<reduction::sum>>},
        {"multiply", make<reduce<reduction::product>>},
        {"mix", make<reduce<reduction::mix>>},
        {"pan", make<pan>},
        {"crossfade", make<crossfade>},
        {"gain", make<elementwise<gainOp>>},
        {"clip", make<elementwise<clipOp>>},
        {"clamp", make<elementwise<clipOp>>},
        {"dbToAmp", make<elementwise<dbToAmpOp>>},
        {"beatsToSeconds", make<elementwise<beatsToSecondsOp>>},
        {"neg", make<elementwise<negOp>>},
        {"abs", make<elementwise<absOp>>},
        {"exp", make<elementwise<expOp>>},
        {"log", make<elementwise<logOp>>},
        {"sin", make<elementwise<sinOp>>},
        {"cos", make<elementwise<cosOp>>},
        {"tanh", make<elementwise<tanhOp>>},
        {"floor", make<elementwise<floorOp>>},
        {"round", make<elementwise<roundOp>>},
        {"subtract", make<elementwise<subtractOp>>},
        {"divide", make<elementwise<divideOp>>},
        {"pow", make<elementwise<powOp>>},
        {"min", make<elementwise<minOp>>},
        {"max", make<elementwise<maxOp>>},
        {"quantize", make<elementwise<quantizeOp>>},
        {"scale", make<elementwise<scaleOp>>},
        {"scaleExp", make<elementwise<scaleExpOp>>},
    };
    return factories;
}

} // namespace primitives
