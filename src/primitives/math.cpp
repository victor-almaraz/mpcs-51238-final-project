// Math, routing, conversion, and mapper operations (primitives.md §7.6,
// §7.9). Each works on audio pipes sample by sample and on control pipes
// value by value, matching its output.

#include "node.h"

#include <cmath>

namespace primitives {

namespace {

// --- Element-wise operations ---------------------------------------------

// f(x, parameters) for one sample or value.
using opFn = float (*)(float x, const float* p, double tempo) noexcept;

struct opSpec {
    std::string_view type;
    std::array<std::string_view, 4> params;  // empty names are unused
    opFn fn;
};

float scaleLinear(float x, const float* p) noexcept {
    const float span = p[1] - p[0];
    const float t = span == 0.0f ? 0.0f : (x - p[0]) / span;
    return p[2] + t * (p[3] - p[2]);
}

float scaleExponential(float x, const float* p) noexcept {
    const float span = p[1] - p[0];
    const float t = span == 0.0f ? 0.0f : (x - p[0]) / span;
    // Output bounds of the same sign (§7.9.1); otherwise linear.
    if (p[2] == 0.0f || p[3] == 0.0f || (p[2] < 0.0f) != (p[3] < 0.0f)) return p[2] + t * (p[3] - p[2]);
    return p[2] * std::pow(p[3] / p[2], t);
}

constexpr opSpec ops[] = {
    {"gain", {"amount"}, [](float x, const float* p, double) noexcept { return x * p[0]; }},
    {"clip", {"low", "high"}, [](float x, const float* p, double) noexcept { return std::clamp(x, std::min(p[0], p[1]), std::max(p[0], p[1])); }},
    {"clamp", {"low", "high"}, [](float x, const float* p, double) noexcept { return std::clamp(x, std::min(p[0], p[1]), std::max(p[0], p[1])); }},
    {"dbToAmp", {}, [](float x, const float*, double) noexcept { return std::pow(10.0f, x / 20.0f); }},
    {"beatsToSeconds", {}, [](float x, const float*, double tempo) noexcept { return static_cast<float>(x * 60.0 / tempo); }},
    {"neg", {}, [](float x, const float*, double) noexcept { return -x; }},
    {"abs", {}, [](float x, const float*, double) noexcept { return std::abs(x); }},
    {"exp", {}, [](float x, const float*, double) noexcept { return std::exp(x); }},
    {"log", {}, [](float x, const float*, double) noexcept { return x > 0.0f ? std::log(x) : -1e30f; }},
    {"sin", {}, [](float x, const float*, double) noexcept { return std::sin(x); }},
    {"cos", {}, [](float x, const float*, double) noexcept { return std::cos(x); }},
    {"tanh", {}, [](float x, const float*, double) noexcept { return std::tanh(x); }},
    {"floor", {}, [](float x, const float*, double) noexcept { return std::floor(x); }},
    {"round", {}, [](float x, const float*, double) noexcept { return std::round(x); }},
    {"subtract", {"b"}, [](float x, const float* p, double) noexcept { return x - p[0]; }},
    {"divide", {"b"}, [](float x, const float* p, double) noexcept { return p[0] == 0.0f ? 0.0f : x / p[0]; }},
    {"pow", {"b"}, [](float x, const float* p, double) noexcept { return std::pow(x, p[0]); }},
    {"min", {"b"}, [](float x, const float* p, double) noexcept { return std::min(x, p[0]); }},
    {"max", {"b"}, [](float x, const float* p, double) noexcept { return std::max(x, p[0]); }},
    {"quantize", {"step"}, [](float x, const float* p, double) noexcept { return p[0] > 0.0f ? std::round(x / p[0]) * p[0] : x; }},
    {"scale", {"in-low", "in-high", "out-low", "out-high"}, [](float x, const float* p, double) noexcept { return scaleLinear(x, p); }},
    {"scaleExp", {"in-low", "in-high", "out-low", "out-high"}, [](float x, const float* p, double) noexcept { return scaleExponential(x, p); }},
};

const opSpec& findOp(std::string_view type) {
    return *std::find_if(std::begin(ops), std::end(ops), [&](const opSpec& o) { return o.type == type; });
}

class elementwise final : public primitive {
public:
    explicit elementwise(nodeContext& c)
        : primitive(c.id(), c.type()), op_(findOp(c.type())), in_(c.signalIn("in")), out_(c.signalOutput("out")) {
        for (std::size_t k = 0; k < op_.params.size() && !op_.params[k].empty(); ++k) {
            params_[k] = c.param(op_.params[k]);
            ++count_;
        }
    }

    void prepare(const prepareInfo& info) override { buffer_.assign(static_cast<std::size_t>(info.blockSize), 0.0f); }

    void process(const blockContext& ctx) noexcept override {
        const int n = ctx.numSamples;
        std::array<float, 4> p{};
        for (int ch = 0; ch < out_.channels; ++ch) {
            if (out_.control) {
                for (std::size_t k = 0; k < count_; ++k) p[k] = params_[k].value(ch);
                emit(*out_.control, ch, op_.fn(in_.value(ch), p.data(), ctx.tempo));
                continue;
            }
            const auto x = in_.block(ch, n);
            std::array<std::span<const float>, 4> blocks;
            for (std::size_t k = 0; k < count_; ++k) blocks[k] = params_[k].block(ch, n);
            for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) {
                for (std::size_t k = 0; k < count_; ++k) p[k] = blocks[k][i];
                buffer_[i] = op_.fn(x[i], p.data(), ctx.tempo);
            }
            emit(*out_.audio, ch, {buffer_.data(), static_cast<std::size_t>(n)});
        }
    }

private:
    const opSpec& op_;
    signal in_;
    signalOut out_;
    std::array<signal, 4> params_;
    std::size_t count_ = 0;
    std::vector<float> buffer_;
};

// --- Reductions over several inputs: add, multiply, mix -----------------

enum class reduction { sum, product, mix };

template <reduction R>
class reduce final : public primitive {
public:
    explicit reduce(nodeContext& c) : primitive(c.id(), c.type()), out_(c.signalOutput("out")) {
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

    void prepare(const prepareInfo& info) override { buffer_.assign(static_cast<std::size_t>(info.blockSize), 0.0f); }

    void process(const blockContext& ctx) noexcept override {
        const int n = ctx.numSamples;
        constexpr float identity = R == reduction::product ? 1.0f : 0.0f;
        for (int ch = 0; ch < out_.channels; ++ch) {
            if (out_.control) {
                float acc = identity;
                for (std::size_t k = 0; k < inputs_.size(); ++k) combine(acc, inputs_[k].value(ch), levels_[k]);
                emit(*out_.control, ch, acc);
                continue;
            }
            std::fill(buffer_.begin(), buffer_.begin() + n, identity);
            for (std::size_t k = 0; k < inputs_.size(); ++k) {
                const auto x = inputs_[k].block(ch, n);
                for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) combine(buffer_[i], x[i], levels_[k]);
            }
            emit(*out_.audio, ch, {buffer_.data(), static_cast<std::size_t>(n)});
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
    std::vector<float> buffer_;
};

// --- pan ----------------------------------------------------------------

class pan final : public primitive {
public:
    explicit pan(nodeContext& c)
        : primitive(c.id(), c.type()), in_(c.signalIn("in")), position_(c.param("position")), out_(c.audioOut("out")) {
        if (c.outputChannels("out") < 2) c.fail("pan needs an output with two channels");
    }

    void prepare(const prepareInfo& info) override {
        left_.assign(static_cast<std::size_t>(info.blockSize), 0.0f);
        right_.assign(left_.size(), 0.0f);
    }

    void process(const blockContext& ctx) noexcept override {
        const int n = ctx.numSamples;
        const auto x = in_.block(0, n);
        const auto pos = position_.block(0, n);
        for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) {
            const double angle = (std::clamp(pos[i], -1.0f, 1.0f) + 1.0) * pi / 4.0;  // equal power
            left_[i] = static_cast<float>(x[i] * std::cos(angle));
            right_[i] = static_cast<float>(x[i] * std::sin(angle));
        }
        emit(*out_, 0, {left_.data(), static_cast<std::size_t>(n)});
        emit(*out_, 1, {right_.data(), static_cast<std::size_t>(n)});
    }

private:
    signal in_;
    signal position_;
    aPipe* out_;
    std::vector<float> left_;
    std::vector<float> right_;
};

// --- crossfade ----------------------------------------------------------

class crossfade final : public primitive {
public:
    explicit crossfade(nodeContext& c)
        : primitive(c.id(), c.type()),
          a_(c.signalIn("a")),
          b_(c.signalIn("b")),
          position_(c.param("position")),
          linear_(c.choice("law") == 1),
          out_(c.signalOutput("out")) {}

    void prepare(const prepareInfo& info) override { buffer_.assign(static_cast<std::size_t>(info.blockSize), 0.0f); }

    void process(const blockContext& ctx) noexcept override {
        const int n = ctx.numSamples;
        for (int ch = 0; ch < out_.channels; ++ch) {
            if (out_.control) {
                emit(*out_.control, ch, mixAt(a_.value(ch), b_.value(ch), position_.value(ch)));
                continue;
            }
            const auto a = a_.block(ch, n);
            const auto b = b_.block(ch, n);
            const auto pos = position_.block(ch, n);
            for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) buffer_[i] = mixAt(a[i], b[i], pos[i]);
            emit(*out_.audio, ch, {buffer_.data(), static_cast<std::size_t>(n)});
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
    bool linear_;
    signalOut out_;
    std::vector<float> buffer_;
};

} // namespace

std::span<const factoryEntry> mathFactories() {
    static const std::vector<factoryEntry> all = [] {
        std::vector<factoryEntry> v{
            {"add", make<reduce<reduction::sum>>},
            {"multiply", make<reduce<reduction::product>>},
            {"mix", make<reduce<reduction::mix>>},
            {"pan", make<pan>},
            {"crossfade", make<crossfade>},
        };
        for (const auto& op : ops) v.push_back({op.type, make<elementwise>});
        return v;
    }();
    return all;
}

} // namespace primitives
