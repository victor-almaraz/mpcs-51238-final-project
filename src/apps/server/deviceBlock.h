// One audio device block: device channel arrays to engine spans. Has no
// JUCE dependency, so it is tested without devices. Runs on the audio
// device thread: it allocates nothing.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <espresso/primitives/engine.h>
#include <espresso/primitives/realtime.h>

namespace server {

/// Processes one block of @p frames frames on @p engine at device time
/// @p now. @p in and @p out are the driver's channel arrays: the narrow
/// unsafe use at the driver boundary (see the modern-cpp skill), where
/// each channel arrives as a raw sample pointer, possibly null. They
/// become spans here, on the stack, so the engine never sees a pointer.
/// The engine drives the first @p driven output channels, at most
/// primitives::maxDeviceChannels; the others are silenced, since the
/// driver plays whatever its buffers hold. With @p driven 0 the device
/// only clocks the engine.
inline void runDeviceBlock(primitives::engine& engine, std::span<const float* const> in,
                           std::span<float* const> out, std::size_t frames, double now,
                           std::size_t driven = SIZE_MAX) noexcept ESPRESSO_NONBLOCKING {
    constexpr auto most = static_cast<std::size_t>(primitives::maxDeviceChannels);
    const auto inputs = std::min(in.size(), most);
    const auto outputs = std::min({out.size(), most, driven});
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
    engine.process({.inputs = std::span(ins).first(inputs), .outputs = std::span(outs).first(outputs)},
                   static_cast<int>(frames), now);
}

}  // namespace server
