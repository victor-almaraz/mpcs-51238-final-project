// Offline rendering and file input for the server: reading IR text,
// rendering a plan without a device, and writing WAVE files
// (primitives.md §11.2). Has no JUCE dependency.

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <proj/primitives/engine.h>

namespace server {

/// The contents of @p path, or of standard input when @p path is "-".
/// Nothing when the file cannot be read.
std::optional<std::string> readText(const std::string& path);

/// Plays @p plan on @p audio without a device for @p frames frames and
/// returns @p outputs channels of output. @p audio must have no active
/// plan; the same plan and seed always give the same samples.
std::vector<std::vector<float>> renderOffline(primitives::engine& audio, std::shared_ptr<primitives::plan> plan,
                                              std::size_t frames, int outputs);

/// Bytes of a WAVE file written by writeWav() before its samples: the
/// RIFF header, an 18-byte `fmt ` chunk, a `fact` chunk, and the `data`
/// chunk header.
inline constexpr std::size_t wavHeaderBytes = 12 + 26 + 12 + 8;

/// The most frames of @p channels channels (at least 1) that writeWav()
/// can store: WAVE sizes are 32-bit.
std::size_t maxWavFrames(int channels) noexcept;

/// Writes @p channels as an interleaved 32-bit float WAVE file
/// (WAVE_FORMAT_IEEE_FLOAT, with a `fact` chunk). Returns false unless
/// every byte reached the file and it closed cleanly, or when the
/// channels do not fit (maxWavFrames()).
bool writeWav(const std::string& path, const std::vector<std::vector<float>>& channels, double sampleRate);

} // namespace server
