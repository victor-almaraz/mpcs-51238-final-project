// Internal: what a primitive's constructor reads from its node, and the
// per-block signal helpers that generators share. Not part of the public
// API; primitives are created through buildPlan().

#pragma once

#include <array>
#include <functional>
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <proj/metadata/graph.h>
#include <proj/metadata/primitiveSpec.h>
#include <proj/primitives/io.h>
#include <proj/primitives/pipeTable.h>
#include <proj/primitives/plan.h>
#include <proj/primitives/primitive.h>
#include <proj/primitives/random.h>

namespace primitives {

inline constexpr int maxChannels = metadata::maxChannels;

// --- Signals ------------------------------------------------------------

// A parameter or input that is a static value, a control pipe, or an
// audio pipe (primitives.md §7.1.2). Read it once per channel per block.
class signal {
public:
    void setConstant(float v) noexcept { constant_ = v; }
    void bind(std::shared_ptr<const aPipe> p) noexcept { audio_ = std::move(p); }
    void bind(std::shared_ptr<const cPipe> p) noexcept { control_ = std::move(p); }

    bool isAudio() const noexcept { return audio_ != nullptr; }
    bool isStatic() const noexcept { return !audio_ && !control_; }
    int channels() const noexcept {
        return audio_ ? audio_->channels() : (control_ ? control_->channels() : 1);
    }

    void prepare(int blockSize) {
        scratch_.assign(static_cast<std::size_t>(blockSize), 0.0f);
        filledWith_ = NAN;
    }

    // The block-rate value of @p ch: the constant, the control value, or
    // the audio pipe's last sample.
    float value(int ch = 0) const noexcept {
        if (control_) return control_->value(mapChannel(ch));
        if (audio_) {
            const auto s = audio_->read(mapChannel(ch));
            return s.empty() ? 0.0f : s.back();
        }
        return constant_;
    }

    // Per-sample values of @p ch for @p n samples. Control values ramp
    // linearly from the previous block's value (§5.3.3).
    std::span<const float> block(int ch, int n) noexcept {
        const auto count = static_cast<std::size_t>(n);
        if (audio_) {
            if (ch < audio_->channels() || audio_->channels() == 1) return audio_->read(mapChannel(ch)).first(count);
            std::fill(scratch_.begin(), scratch_.begin() + n, 0.0f);
            filledWith_ = 0.0f;
            filledCount_ = count;
            return std::span<const float>(scratch_).first(count);
        }
        const float target = value(ch);
        float& last = last_[static_cast<std::size_t>(ch)];
        if (std::isnan(last) || !control_) last = target;
        if (last == target) {
            // Constant over the block: the scratch already holds it unless
            // the value or the block grew since the last fill.
            if (filledWith_ != target || filledCount_ < count) {
                std::fill(scratch_.begin(), scratch_.begin() + static_cast<std::ptrdiff_t>(count), target);
                filledWith_ = target;
                filledCount_ = count;
            }
            return std::span<const float>(scratch_).first(count);
        }
        const float step = (target - last) / static_cast<float>(n > 0 ? n : 1);
        for (std::size_t i = 0; i < count; ++i) scratch_[i] = last + step * static_cast<float>(i + 1);
        last = target;
        filledWith_ = NAN;
        return std::span<const float>(scratch_).first(count);
    }

    // Keeps ramps continuous across a swap.
    void adopt(const signal& other) noexcept { last_ = other.last_; }

private:
    int mapChannel(int ch) const noexcept {
        const int c = channels();
        return c == 1 ? 0 : (ch < c ? ch : c - 1);
    }

    float constant_ = 0.0f;
    std::shared_ptr<const aPipe> audio_;
    std::shared_ptr<const cPipe> control_;
    std::vector<float> scratch_;
    float filledWith_ = NAN;  // the value every scratch sample holds, or NaN
    std::size_t filledCount_ = 0;
    std::array<float, maxChannels> last_{NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};
};

// An output that is an audio or a control pipe.
struct signalOut {
    std::shared_ptr<aPipe> audio;
    std::shared_ptr<cPipe> control;
    int channels = 1;
    explicit operator bool() const noexcept { return audio || control; }
};

// --- Node context -------------------------------------------------------

// Reads a node's parameters and ports, resolving pipes in the table.
// Throws buildError for anything the engine cannot run.
class nodeContext {
public:
    nodeContext(const metadata::nodeSpec& node, const metadata::primitiveSpec& spec,
                const metadata::graphSpec& graph, buildEnv& env)
        : node_(node), spec_(spec), graph_(graph), env_(env) {}

    const std::string& id() const noexcept { return node_.id; }
    std::string_view type() const noexcept { return spec_.type; }
    const prepareInfo& info() const noexcept { return env_.info; }
    std::uint64_t seed() const noexcept { return rng::seedFor(graph_.seed, node_.id); }
    ioHub& io() noexcept { return env_.io; }

    [[noreturn]] void fail(const std::string& message) const { throw buildError(node_.id, message); }

    double number(std::string_view name) const;
    std::string text(std::string_view name) const;
    // Index of the parameter's choice in its spec's list.
    int choice(std::string_view name) const;
    std::vector<double> list(std::string_view name) const;
    signal param(std::string_view name);

    bool hasInput(std::string_view port) const { return node_.inputs.count(port) != 0; }
    bool hasOutput(std::string_view port) const { return node_.outputs.count(port) != 0; }

    // The pipe bound to a port, or null when the port is unbound.
    std::shared_ptr<const aPipe> audioIn(std::string_view port);
    std::shared_ptr<aPipe> audioOut(std::string_view port);
    std::shared_ptr<const ePipe> eventIn(std::string_view port);
    std::shared_ptr<ePipe> eventOut(std::string_view port);
    std::vector<std::shared_ptr<ePipe>> eventOuts(std::string_view port);
    std::shared_ptr<cPipe> controlOut(std::string_view port);
    signal signalIn(std::string_view port, std::size_t which = 0);
    std::size_t inputCount(std::string_view port) const;
    signalOut signalOutput(std::string_view port);

    // Channels declared for the pipe bound to @p port, or 1.
    int outputChannels(std::string_view port) const;
    // Records a port so the plan activates it.
    template <typename P>
    void keep(const std::shared_ptr<P>& port) {
        ports_.push_back(port);
    }
    std::vector<std::shared_ptr<ioPort>> takePorts() { return std::move(ports_); }

private:
    metadata::optionalRef<metadata::paramValue> value(std::string_view name) const;
    template <typename P>
    std::shared_ptr<P> bound(bool input, std::string_view port, metadata::pipeKind kind, std::string_view what);
    const metadata::paramSpec& paramSpec(std::string_view name) const;
    std::shared_ptr<pipe> resolve(metadata::pipeRef ref) const;
    metadata::optionalRef<std::vector<metadata::pipeRef>> refs(bool input, std::string_view port) const;
    int declaredChannels(metadata::pipeRef ref) const;

    const metadata::nodeSpec& node_;
    const metadata::primitiveSpec& spec_;
    const metadata::graphSpec& graph_;
    buildEnv& env_;
    std::vector<std::shared_ptr<ioPort>> ports_;
};

// --- Factories ----------------------------------------------------------

using factory = std::function<std::shared_ptr<primitive>(nodeContext&)>;

struct factoryEntry {
    std::string_view type;
    factory make;
};

template <typename T>
std::shared_ptr<primitive> make(nodeContext& c) {
    return std::make_shared<T>(c);
}

// One table per source file.
std::span<const factoryEntry> oscillatorFactories();
std::span<const factoryEntry> filterFactories();
std::span<const factoryEntry> modulationFactories();
std::span<const factoryEntry> delayFactories();
std::span<const factoryEntry> mathFactories();
std::span<const factoryEntry> eventFactories();
std::span<const factoryEntry> deviceFactories();

// --- Shared math --------------------------------------------------------

inline constexpr double pi = 3.14159265358979323846;
inline constexpr double twoPi = 2.0 * pi;

// Swaps two state containers when their sizes match (adoption, §9.2).
template <typename T>
void adoptIfSameSize(T& mine, T& theirs) noexcept {
    if (mine.size() == theirs.size()) std::swap(mine, theirs);
}

} // namespace primitives
