#include <proj/primitives/frame.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace primitives {

frame::frame(int width, int height)
    : width_(std::max(width, 1)), height_(std::max(height, 1)),
      pixels_(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 0u) {}

std::uint32_t frame::at(int x, int y) const noexcept PROJ_NONBLOCKING {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return 0u;
    return pixels_[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width_)) + static_cast<std::size_t>(x)];
}

void frame::clear() noexcept PROJ_NONBLOCKING {
    std::ranges::fill(pixels_, 0u);
    transform_ = {};
}

void frame::copyFrom(const frame& other) noexcept PROJ_NONBLOCKING {
    transform_ = other.transform_;
    if (other.width_ == width_ && other.height_ == height_) {
        std::ranges::copy(other.pixels_, pixels_.begin());
        return;
    }
    std::ranges::fill(pixels_, 0u);
    const auto w = static_cast<std::size_t>(std::min(width_, other.width_));
    for (int y = 0; y < std::min(height_, other.height_); ++y) {
        const auto from = std::span(other.pixels_).subspan(static_cast<std::size_t>(y) * static_cast<std::size_t>(other.width_), w);
        std::ranges::copy(from, std::span(pixels_).subspan(static_cast<std::size_t>(y) * static_cast<std::size_t>(width_)).begin());
    }
}

} // namespace primitives
