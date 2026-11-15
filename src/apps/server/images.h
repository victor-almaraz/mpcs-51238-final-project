// Image files for the server: decoding imagePlayer files and writing
// frames as PNG (primitives.md §7.7.6). Has no JUCE dependency.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <proj/primitives/frame.h>
#include <proj/primitives/plan.h>

namespace server {

/// Decodes the image file at @p path (PNG, JPEG, and the other formats
/// stb_image reads) into premultiplied pixels. Returns no image, with
/// @p error set, when it cannot.
std::optional<primitives::image> loadImageFile(const std::filesystem::path& path, std::string& error);

/// A loader for the engine that resolves relative paths against @p root,
/// the project root (§7.7.6).
primitives::imageLoader imageLoaderFor(std::filesystem::path root);

/// Writes @p f as an 8-bit RGBA PNG file. Returns false when it cannot.
bool writePng(const std::filesystem::path& path, const primitives::frame& f);

}  // namespace server
