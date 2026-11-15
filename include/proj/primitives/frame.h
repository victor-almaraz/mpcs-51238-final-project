/// @file
/// A video frame: the pixels a `vPipe` holds (`primitives.md` §5.5).

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <proj/primitives/realtime.h>

namespace primitives {

/// A 2D affine transform, mapping (x, y) to
/// (xx·x + xy·y + x0, yx·x + yy·y + y0). The identity by default.
struct affine {
    /// Scale and rotation, first column.
    double xx = 1.0;
    /// Scale and rotation, first column.
    double yx = 0.0;
    /// Scale and rotation, second column.
    double xy = 0.0;
    /// Scale and rotation, second column.
    double yy = 1.0;
    /// Translation.
    double x0 = 0.0;
    /// Translation.
    double y0 = 0.0;

    friend bool operator==(const affine&, const affine&) = default;
};

/// Frame pixels: premultiplied alpha, 32 bits per pixel as `0xAARRGGBB`
/// (BGRA in memory on little-endian hosts), rows top to bottom with no
/// padding. A frame also carries the transform that drawing applies
/// (§7.7.2). Storage is allocated at construction; nothing allocates
/// afterwards.
class frame {
public:
    /// A transparent frame of @p width × @p height pixels, each at least 1.
    frame(int width, int height);

    /// Width, in pixels.
    int width() const noexcept { return width_; }
    /// Height, in pixels.
    int height() const noexcept { return height_; }

    /// Every pixel, row by row.
    std::span<std::uint32_t> pixels() noexcept { return pixels_; }
    /// Every pixel, row by row.
    std::span<const std::uint32_t> pixels() const noexcept { return pixels_; }
    /// The pixel at (@p x, @p y); transparent outside the frame.
    std::uint32_t at(int x, int y) const noexcept PROJ_NONBLOCKING;

    /// The transform drawing applies.
    const affine& transform() const noexcept { return transform_; }
    /// Replaces the transform drawing applies.
    void setTransform(const affine& t) noexcept { transform_ = t; }

    /// Makes every pixel transparent and resets the transform.
    void clear() noexcept PROJ_NONBLOCKING;
    /// Copies @p other's pixels and transform. Frames of different sizes
    /// copy the overlapping region and clear the rest.
    void copyFrom(const frame& other) noexcept PROJ_NONBLOCKING;

private:
    int width_;
    int height_;
    std::vector<std::uint32_t> pixels_;
    affine transform_;
};

} // namespace primitives
