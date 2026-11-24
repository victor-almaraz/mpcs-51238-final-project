#include "images.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/primitives/frame.h>
#include <espresso/primitives/plan.h>

#include <stb_image.h>
#include <stb_image_write.h>

namespace server {

namespace {

// stb_image returns pixels it allocated, by pointer: owned here until
// stbi_image_free (the narrow unsafe use at this library boundary, the
// modern-cpp skill's section 9).
struct stbFree {
    void operator()(unsigned char* pixels) const noexcept { stbi_image_free(pixels); }
};

std::uint32_t premultiplied(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) noexcept {
    const auto scale = [a](std::uint32_t c) { return ((c * a) + 127u) / 255u; };
    return (a << 24u) | (scale(r) << 16u) | (scale(g) << 8u) | scale(b);
}

std::uint8_t straight(std::uint32_t c, std::uint32_t a) noexcept {
    return static_cast<std::uint8_t>(a == 0u ? 0u : std::min(255u, ((c * 255u) + (a / 2u)) / a));
}

}  // namespace

std::optional<primitives::image> loadImageFile(const std::filesystem::path& path, std::string& error) {
    int w = 0;
    int h = 0;
    int channels = 0;
    const std::unique_ptr<unsigned char, stbFree> pixels(stbi_load(path.c_str(), &w, &h, &channels, 4));
    if (!pixels) {
        error = stbi_failure_reason() != nullptr ? stbi_failure_reason() : "unreadable image";
        return std::nullopt;
    }
    const std::span<const unsigned char> rgba(pixels.get(),
                                              static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    primitives::image out{.width = w, .height = h, .pixels = {}};
    out.pixels.reserve(rgba.size() / 4);
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        out.pixels.push_back(premultiplied(rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]));
    }
    return out;
}

primitives::imageLoader imageLoaderFor(std::filesystem::path root) {
    return [root = std::move(root)](std::string_view path, std::string& error) {
        // An absolute path replaces the root (std::filesystem::path::operator/).
        return loadImageFile(root / path, error);
    };
}

bool writePng(const std::filesystem::path& path, const primitives::frame& f) {
    std::vector<std::uint8_t> rgba;
    rgba.reserve(f.pixels().size() * 4);
    for (const std::uint32_t p : f.pixels()) {
        const std::uint32_t a = p >> 24u;
        rgba.push_back(straight((p >> 16u) & 0xffu, a));
        rgba.push_back(straight((p >> 8u) & 0xffu, a));
        rgba.push_back(straight(p & 0xffu, a));
        rgba.push_back(static_cast<std::uint8_t>(a));
    }
    const int stride = f.width() * 4;  // bytes per row
    return stbi_write_png(path.c_str(), f.width(), f.height(), 4, rgba.data(), stride) != 0;
}

}  // namespace server
