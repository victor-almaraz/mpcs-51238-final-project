// Offline rendering and file input for the server: reading IR text,
// rendering a plan without a device, and writing WAVE files
// (primitives.md §11.2). Has no JUCE dependency.

#pragma once

#include <cstddef>
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

/// True when @p frames frames of @p channels channels fit in a WAVE file,
/// whose sizes are 32-bit.
bool fitsWav(std::size_t frames, int channels) noexcept;

/// Writes @p channels as an interleaved 32-bit float WAVE file. Returns
/// false when the file cannot be written.
bool writeWav(const std::string& path, const std::vector<std::vector<float>>& channels, double sampleRate);

} // namespace server
