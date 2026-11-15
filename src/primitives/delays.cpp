// Delays: interpolated delay line, comb, and allpass (primitives.md §7.6).
// Each channel has its own circular buffer, sized from max-time when the
// node is built (§7.4.2).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <proj/primitives/primitive.h>
#include <proj/primitives/realtime.h>

#include "node.h"

namespace primitives {

namespace {

enum class interpolation : std::uint8_t { linear, none, cubic };

// One circular buffer per channel.
class delayBuffers {
public:
    // Room for @p maxSamples of delay on each of @p channels.
    delayBuffers(int channels, double maxSamples)
        // Room for the longest delay plus the cubic interpolator's taps.
        : lines_(static_cast<std::size_t>(channels),
                 std::vector<float>(static_cast<std::size_t>(std::ceil(maxSamples)) + 4, 0.0f)) {}

    // Takes the other buffers when they have the same shape and hold no
    // non-finite sample.
    void adopt(delayBuffers& other) noexcept {
        if (other.poisoned_ == 0 && lines_.size() == other.lines_.size() && size() == other.size()) {
            std::swap(lines_, other.lines_);
            write_ = other.write_;
        }
    }

    std::size_t size() const noexcept { return lines_.empty() ? 0 : lines_[0].size(); }

    // The sample @p delay samples before the write position of @p ch.
    float read(int ch, double delay, interpolation mode) const noexcept {
        const auto& line = lines_[static_cast<std::size_t>(ch)];
        const auto size = static_cast<double>(line.size());
        double pos = static_cast<double>(write_) - delay;
        if (pos < 0.0) pos += size;
        const auto i = static_cast<std::size_t>(pos);
        const auto frac = static_cast<float>(pos - static_cast<double>(i));
        auto at = [&](std::size_t k) { return line[k % line.size()]; };
        switch (mode) {
        case interpolation::none: return at(static_cast<std::size_t>(std::lround(pos)));
        case interpolation::linear: return at(i) + frac * (at(i + 1) - at(i));
        case interpolation::cubic: {  // 4-point Hermite
            const float xm1 = at(i + line.size() - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
            const float c1 = 0.5f * (x1 - xm1);
            const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            return ((c3 * frac + c2) * frac + c1) * frac + x0;
        }
        }
        return 0.0f;
    }

    void write(int ch, float v) noexcept { lines_[static_cast<std::size_t>(ch)][write_] = v; }

    // Advances every channel's write position by one sample.
    void advance() noexcept {
        if (++write_ >= size()) write_ = 0;
    }

    // Rewinds before a channel's pass, so every channel sees one block.
    std::size_t position() const noexcept { return write_; }
    void seek(std::size_t p) noexcept { write_ = p; }

    // Ends a block of @p n samples. @p finite is false when the block
    // wrote a non-finite sample: the buffers then hold one until every
    // position has been written again.
    void endBlock(bool finite, std::size_t n) noexcept {
        poisoned_ = !finite ? size() : (poisoned_ > n ? poisoned_ - n : 0);
    }

private:
    std::vector<std::vector<float>> lines_;
    std::size_t write_ = 0;
    // Samples until the last non-finite write is overwritten; 0 when none.
    std::size_t poisoned_ = 0;
};

// The three delays differ only in the per-sample recurrence.
enum class kind : std::uint8_t { line, comb, allpass };

template <kind K>
class delay final : public nodeBase {
public:
    explicit delay(nodeContext& c)
        : nodeBase(c),
          in_(c.signalIn("in")),
          out_(c.audioOut("out")),
          time_(c.param("time")),
          feedback_(c.paramOr("feedback", 0.0f)),
          damping_(c.paramOr("damping", 0.0f)),
          mode_(K == kind::line ? static_cast<interpolation>(c.choice("interpolation"))
                                : interpolation::linear),
          sampleRate_(c.sampleRate()),
          // At least one sample, the shortest delay (process()).
          maxSamples_(std::max(c.number("max-time") * c.sampleRate(), 1.0)),
          lines_(out_.channels, maxSamples_),
          buffer_(c.blockSize()) {}

    void adoptState(primitive& outgoing) noexcept PROJ_NONBLOCKING override {
        auto& o = static_cast<delay&>(outgoing);
        lines_.adopt(o.lines_);
        adoptFinite(damped_, o.damped_);
    }

    void process(const blockContext& ctx) noexcept PROJ_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto count = static_cast<std::size_t>(n);
        const std::size_t start = lines_.position();
        // Comb and allpass read before writing, so their shortest delay is
        // one sample; so is cubic interpolation's, whose last tap would
        // otherwise reach past the newest sample.
        const double shortest = K == kind::line && mode_ != interpolation::cubic ? 0.0 : 1.0;
        // v × 0 is 0 for a finite v and NaN otherwise: summing it over the
        // written samples detects a non-finite write without a branch.
        float written = 0.0f;
        for (int ch = 0; ch < out_.channels; ++ch) {
            lines_.seek(start);
            const auto x = in_.block(ch, n);
            const auto t = time_.block(ch, n);
            const auto fb = feedback_.block(ch, n);
            const auto damp = damping_.block(ch, n);
            float& damped = damped_[static_cast<std::size_t>(ch)];
            for (std::size_t i = 0; i < count; ++i) {
                // fmax and fmin, unlike clamp, turn a NaN time into the shortest delay.
                const double d =
                    std::fmin(std::fmax(static_cast<double>(t[i]) * sampleRate_, shortest), maxSamples_);
                float w = 0.0f;
                if constexpr (K == kind::line) {
                    w = x[i];
                    lines_.write(ch, w);
                    buffer_[i] = lines_.read(ch, d, mode_);
                } else if constexpr (K == kind::comb) {
                    const float y = lines_.read(ch, d, mode_);
                    const float a = std::clamp(damp[i], 0.0f, 1.0f);
                    damped = y * (1.0f - a) + damped * a;
                    w = x[i] + damped * std::clamp(fb[i], -1.0f, 1.0f);
                    lines_.write(ch, w);
                    buffer_[i] = y;
                } else {
                    const float g = std::clamp(fb[i], -1.0f, 1.0f);
                    const float delayed = lines_.read(ch, d, mode_);
                    w = x[i] + g * delayed;
                    lines_.write(ch, w);
                    buffer_[i] = delayed - g * w;
                }
                written += w * 0.0f;
                lines_.advance();
            }
            emit(out_, ch, buffer_.first(count));
        }
        lines_.endBlock(std::isfinite(written), count);
    }

private:
    signal in_;
    audioOutput out_;
    signal time_;
    signal feedback_;
    signal damping_;
    interpolation mode_ = interpolation::linear;
    double sampleRate_ = 48000.0;
    double maxSamples_ = 1.0;
    delayBuffers lines_;
    std::array<float, maxChannels> damped_{};
    blockBuffer buffer_;
};

} // namespace

std::span<const factoryEntry> delayFactories() {
    static const auto factories = std::to_array<factoryEntry>({
        {.type = "delayLine", .make = make<delay<kind::line>>},
        {.type = "comb", .make = make<delay<kind::comb>>},
        {.type = "allpass", .make = make<delay<kind::allpass>>},
    });
    return factories;
}

} // namespace primitives
