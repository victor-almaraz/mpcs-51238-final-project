#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <espresso/metadata/primitiveSpec.h>
#include <espresso/primitives/frame.h>
#include <espresso/primitives/realtime.h>

#include "canvas.h"

namespace primitives {

frame::frame(int width, int height)
    : width_(std::max(width, 1)),
      height_(std::max(height, 1)),
      pixels_(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 0u),
      canvas_(std::make_unique<canvas>(*this)) {}

frame::~frame() = default;
// A moved vector keeps its heap storage, so the canvas still draws into it.
frame::frame(frame&&) noexcept = default;

std::uint32_t frame::at(int x, int y) const noexcept ESPRESSO_NONBLOCKING {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return 0u;
    return pixels_[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width_)) +
                   static_cast<std::size_t>(x)];
}

void frame::fill(const metadata::rgba& color) noexcept ESPRESSO_NONBLOCKING { canvas_->fillAll(color); }

frame::frame(const image& source) : frame(source.width, source.height) {
    copyOverlap(source.pixels, source.width, source.height);
}

void frame::clear() noexcept ESPRESSO_NONBLOCKING {
    std::ranges::fill(pixels_, 0u);
    transform_ = {};
}

void frame::copyFrom(const frame& other) noexcept ESPRESSO_NONBLOCKING {
    transform_ = other.transform_;
    if (other.width_ == width_ && other.height_ == height_) {
        std::ranges::copy(other.pixels_, pixels_.begin());
        return;
    }
    std::ranges::fill(pixels_, 0u);
    copyOverlap(other.pixels_, other.width_, other.height_);
}

void frame::copyOverlap(std::span<const std::uint32_t> from, int fromWidth,
                        int fromHeight) noexcept ESPRESSO_NONBLOCKING {
    const auto w = static_cast<std::size_t>(std::min(width_, fromWidth));
    const std::span<std::uint32_t> to(pixels_);
    for (int y = 0; y < std::min(height_, fromHeight); ++y) {
        const auto row = static_cast<std::size_t>(y);
        const auto start = row * static_cast<std::size_t>(fromWidth);
        if (start + w > from.size()) break;  // fewer pixels than the size claims
        std::ranges::copy(from.subspan(start, w), to.subspan(row * static_cast<std::size_t>(width_)).begin());
    }
}

}  // namespace primitives
