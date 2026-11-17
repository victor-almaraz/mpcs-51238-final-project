// Internal: the peak limiter behind audioOut. Instant attack, so no
// sample exceeds the ceiling; exponential release over 50 ms. Linked
// across channels, so limiting does not move the stereo image.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>

#include <espresso/metadata/graph.h>

namespace primitives {

class peakLimiter {
public:
    using inputs = std::array<std::span<const float>, metadata::maxChannels>;

    explicit peakLimiter(double sampleRate)
        : release_(static_cast<float>(std::exp(-1.0 / (0.05 * sampleRate)))) {}

    // True when the gain is finite, so adopting it cannot keep the output
    // silent (§9.2).
    bool isFinite() const noexcept { return std::isfinite(gain_); }

    // Limits @p n samples of @p channels inputs into @p out, indexed by
    // channel and sample. Samples whose peak stays at or below @p ceiling
    // pass unchanged once the gain has recovered to 1.
    template <typename Outputs>
    void process(const inputs& in, int channels, Outputs& out, std::size_t n, float ceiling) noexcept {
        const auto count = static_cast<std::size_t>(channels);
        for (std::size_t i = 0; i < n; ++i) {
            float peak = 0.0f;
            for (std::size_t ch = 0; ch < count; ++ch) peak = std::max(peak, std::abs(in[ch][i]));
            const float target = peak > ceiling ? ceiling / peak : 1.0f;
            gain_ = target < gain_ ? target : target + (gain_ - target) * release_;
            for (std::size_t ch = 0; ch < count; ++ch) out[ch][i] = std::clamp(in[ch][i] * gain_, -ceiling, ceiling);
        }
    }

private:
    float release_ = 0.0f;
    float gain_ = 1.0f;
};

} // namespace primitives
