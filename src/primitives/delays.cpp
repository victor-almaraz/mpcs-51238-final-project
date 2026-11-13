// Delays: interpolated delay line, comb, and allpass (primitives.md §7.6).
// Each channel has its own circular buffer, sized from max-time at
// prepare (§7.4.2).

#include "node.h"

#include <cmath>

namespace primitives {

namespace {

enum class interpolation { linear, none, cubic };

// One circular buffer per channel.
class delayBuffers {
public:
    void prepare(int channels, double maxSamples) {
        // Room for the longest delay plus the cubic interpolator's taps.
        const auto size = static_cast<std::size_t>(std::ceil(maxSamples)) + 4;
        lines_.assign(static_cast<std::size_t>(channels), std::vector<float>(size, 0.0f));
        write_ = 0;
    }

    void adopt(delayBuffers& other) noexcept {
        if (lines_.size() == other.lines_.size() && !lines_.empty() && lines_[0].size() == other.lines_[0].size()) {
            std::swap(lines_, other.lines_);
            write_ = other.write_;
        }
    }

    std::size_t size() const noexcept { return lines_.empty() ? 0 : lines_[0].size(); }

    // The sample @p delay samples before the write position of @p ch.
    float read(int ch, double delay, interpolation mode) const noexcept {
        const auto& line = lines_[static_cast<std::size_t>(ch)];
        const double size = static_cast<double>(line.size());
        double pos = static_cast<double>(write_) - delay;
        if (pos < 0.0) pos += size;
        const auto i = static_cast<std::size_t>(pos);
        const float frac = static_cast<float>(pos - static_cast<double>(i));
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
    void advance() noexcept { write_ = (write_ + 1) % size(); }

    // Rewinds before a channel's pass, so every channel sees one block.
    std::size_t position() const noexcept { return write_; }
    void seek(std::size_t p) noexcept { write_ = p; }

private:
    std::vector<std::vector<float>> lines_;
    std::size_t write_ = 0;
};

// The three delays differ only in the per-sample recurrence.
enum class kind { line, comb, allpass };

template <kind K>
class delay final : public primitive {
public:
    explicit delay(nodeContext& c)
        : primitive(c.id(), c.type()),
          in_(c.signalIn("in")),
          out_(c.audioOut("out")),
          channels_(c.outputChannels("out")),
          time_(c.param("time")),
          maxTime_(c.number("max-time")),
          feedback_(K == kind::line ? signal{} : c.param("feedback")),
          damping_(K == kind::comb ? c.param("damping") : signal{}),
          mode_(K == kind::line ? static_cast<interpolation>(c.choice("interpolation")) : interpolation::linear) {
        feedback_.prepare(c.info().blockSize);
        damping_.prepare(c.info().blockSize);
    }

    void prepare(const prepareInfo& info) override {
        sampleRate_ = info.sampleRate;
        maxSamples_ = maxTime_ * info.sampleRate;
        lines_.prepare(channels_, maxSamples_);
        buffer_.assign(static_cast<std::size_t>(info.blockSize), 0.0f);
    }

    void adoptState(primitive& outgoing) noexcept override {
        auto& o = static_cast<delay&>(outgoing);
        lines_.adopt(o.lines_);
        damped_ = o.damped_;
    }

    void process(const blockContext& ctx) noexcept override {
        const int n = ctx.numSamples;
        const std::size_t start = lines_.position();
        // Comb and allpass read before writing, so their shortest delay is
        // one sample.
        const double shortest = K == kind::line ? 0.0 : 1.0;
        for (int ch = 0; ch < channels_; ++ch) {
            lines_.seek(start);
            const auto x = in_.block(ch, n);
            const auto t = time_.block(ch, n);
            const auto fb = feedback_.block(ch, n);
            const auto damp = damping_.block(ch, n);
            float& damped = damped_[static_cast<std::size_t>(ch)];
            for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) {
                const double d = std::clamp(static_cast<double>(t[i]) * sampleRate_, shortest, maxSamples_);
                if constexpr (K == kind::line) {
                    lines_.write(ch, x[i]);
                    buffer_[i] = lines_.read(ch, d, mode_);
                } else if constexpr (K == kind::comb) {
                    const float y = lines_.read(ch, d, mode_);
                    const float a = std::clamp(damp[i], 0.0f, 1.0f);
                    damped = y * (1.0f - a) + damped * a;
                    lines_.write(ch, x[i] + damped * std::clamp(fb[i], -1.0f, 1.0f));
                    buffer_[i] = y;
                } else {
                    const float g = std::clamp(fb[i], -1.0f, 1.0f);
                    const float delayed = lines_.read(ch, d, mode_);
                    const float w = x[i] + g * delayed;
                    lines_.write(ch, w);
                    buffer_[i] = delayed - g * w;
                }
                lines_.advance();
            }
            emit(*out_, ch, {buffer_.data(), static_cast<std::size_t>(n)});
        }
    }

private:
    signal in_;
    aPipe* out_;
    int channels_;
    signal time_;
    double maxTime_;
    signal feedback_;
    signal damping_;
    interpolation mode_;
    double sampleRate_ = 48000.0;
    double maxSamples_ = 0.0;
    delayBuffers lines_;
    std::array<float, maxChannels> damped_{};
    std::vector<float> buffer_;
};

constexpr factoryEntry factories[] = {
    {"delayLine", make<delay<kind::line>>},
    {"comb", make<delay<kind::comb>>},
    {"allpass", make<delay<kind::allpass>>},
};

} // namespace

std::span<const factoryEntry> delayFactories() { return factories; }

} // namespace primitives
