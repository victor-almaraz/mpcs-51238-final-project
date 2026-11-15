// One audio device block: device channel arrays to engine spans. Has no
// JUCE dependency, so it is tested without devices. Runs on the audio
// device thread: it allocates nothing.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

#include <proj/primitives/engine.h>
#include <proj/primitives/realtime.h>

namespace server {

/// Processes one block of @p frames frames on @p engine at device time
/// @p now. @p in and @p out are the driver's channel arrays: the narrow
/// unsafe use at the driver boundary (see the modern-cpp skill), where
/// each channel arrives as a raw sample pointer, possibly null. They
/// become spans here, on the stack, so the engine never sees a pointer.
/// The engine drives at most primitives::maxDeviceChannels channels each
/// way; output channels beyond that are silenced, since the driver plays
/// whatever its buffers hold.
inline void runDeviceBlock(primitives::engine& engine, std::span<const float* const> in,
                           std::span<float* const> out, std::size_t frames,
                           double now) noexcept PROJ_NONBLOCKING {
    constexpr auto most = static_cast<std::size_t>(primitives::maxDeviceChannels);
    const auto inputs = std::min(in.size(), most);
    const auto outputs = std::min(out.size(), most);
    std::array<std::span<const float>, primitives::maxDeviceChannels> ins{};
    std::array<std::span<float>, primitives::maxDeviceChannels> outs{};
    for (std::size_t c = 0; c < inputs; ++c) {
        if (in[c]) ins[c] = std::span<const float>(in[c], frames);
    }
    for (std::size_t c = 0; c < outputs; ++c) {
        if (out[c]) outs[c] = std::span<float>(out[c], frames);
    }
    for (std::size_t c = outputs; c < out.size(); ++c) {
        if (out[c]) std::fill_n(out[c], frames, 0.0f);
    }
    engine.process({std::span(ins).first(inputs), std::span(outs).first(outputs)}, static_cast<int>(frames),
                   now);
}

}  // namespace server
