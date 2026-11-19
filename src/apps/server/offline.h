// Offline rendering and file input for the server: reading IR text,
// rendering a plan without a device, and writing WAVE files
// (primitives.md §11.2). Has no JUCE dependency.

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <espresso/primitives/buffer.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/frame.h>

namespace server {

/// The contents of @p path, or of standard input when @p path is "-".
/// Nothing when the file cannot be read.
std::optional<std::string> readText(const std::string& path);

/// Receives frame @p index of an offline render, the visual layer's frame
/// at index / frame rate seconds.
using frameSink = std::function<void(std::size_t index, const primitives::frame& f)>;

/// Plays @p plan on @p audio without a device for @p frames frames and
/// returns @p outputs channels of output. With a @p frameRate and
/// @p onFrame, it also renders the frame at each time k / frameRate before
/// the end, after the audio block that reaches that time, and passes the
/// visual layer's frame to @p onFrame. @p audio must have no active plan;
/// the same plan and seed always give the same samples and frames.
std::vector<std::vector<float>> renderOffline(primitives::engine& audio,
                                              std::shared_ptr<primitives::plan> plan, std::size_t frames,
                                              int outputs, double frameRate = 0.0,
                                              const frameSink& onFrame = {},
                                              const std::function<void()>& afterBlock = {});

/// Bytes of a WAVE file written by writeWav() before its samples: the
/// RIFF header, an 18-byte `fmt ` chunk, a `fact` chunk, and the `data`
/// chunk header.
inline constexpr std::size_t wavHeaderBytes = 12 + 26 + 12 + 8;

/// The most frames of @p channels channels (at least 1) that writeWav()
/// can store: WAVE sizes are 32-bit.
std::size_t maxWavFrames(int channels) noexcept;

/// Writes @p channels, of equal length, as an interleaved 32-bit float
/// WAVE file (WAVE_FORMAT_IEEE_FLOAT, with a `fact` chunk). Returns false
/// unless every byte reached the file and it closed cleanly, or when the
/// channels do not fit (maxWavFrames()).
bool writeWav(const std::string& path, std::span<const std::span<const float>> channels, double sampleRate);
/// Writes @p channels as the overload of spans does.
bool writeWav(const std::string& path, std::span<const std::vector<float>> channels, double sampleRate);
/// Writes the channels of @p buffer as the overload of spans does.
bool writeWav(const std::string& path, const primitives::sampleBuffer& buffer, double sampleRate);

} // namespace server
