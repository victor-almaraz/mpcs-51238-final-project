/// @file
/// A video frame: the pixels a `vPipe` holds (`primitives.md` §5.5).

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <proj/metadata/primitiveSpec.h>
#include <proj/primitives/realtime.h>

namespace primitives {

class canvas;

/// A decoded image: pixels as in a frame (premultiplied `0xAARRGGBB`, rows
/// top to bottom).
struct image {
    /// Width, in pixels.
    int width = 0;
    /// Height, in pixels.
    int height = 0;
    /// width × height pixels.
    std::vector<std::uint32_t> pixels;
};

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

    /// True when every coefficient is equal.
    friend bool operator==(const affine&, const affine&) = default;
};

/// Frame pixels: premultiplied alpha, 32 bits per pixel as `0xAARRGGBB`
/// (BGRA in memory on little-endian hosts), rows top to bottom with no
/// padding. A frame also carries the transform that drawing applies
/// (§7.7.2) and a drawing context bound to its pixels. Storage and the
/// context are allocated at construction; nothing allocates afterwards.
class frame {
public:
    /// A transparent frame of @p width × @p height pixels, each at least 1.
    frame(int width, int height);
    /// A frame of @p source's size holding its pixels.
    explicit frame(const image& source);
    ~frame();

    frame(const frame&) = delete;
    frame& operator=(const frame&) = delete;
    /// Takes over @p other's pixels, transform, and canvas.
    frame(frame&& other) noexcept;
    // Not assignable: the defaulted move would free the old pixels before
    // the old canvas bound to them.
    frame& operator=(frame&& other) = delete;

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

    /// Composites @p color, straight alpha, over every pixel.
    void fill(const metadata::rgba& color) noexcept PROJ_NONBLOCKING;

    /// The drawing context bound to the pixels (internal to primitives).
    primitives::canvas& drawing() noexcept { return *canvas_; }

private:
    // Copies the top-left overlap of @p from, @p fromWidth × @p fromHeight
    // pixels, into the same rows and columns here; the rest is unchanged.
    void copyOverlap(std::span<const std::uint32_t> from, int fromWidth,
                     int fromHeight) noexcept PROJ_NONBLOCKING;

    int width_ = 1;
    int height_ = 1;
    std::vector<std::uint32_t> pixels_;  // the canvas draws into this storage: declared first
    affine transform_;
    std::unique_ptr<primitives::canvas> canvas_;
};

}  // namespace primitives
