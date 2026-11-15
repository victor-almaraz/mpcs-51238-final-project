// Drawing into a frame with Blend2D (primitives.md §7.7). Internal to
// primitives, which links Blend2D privately: no Blend2D type appears in a
// public header.

#pragma once

#include <blend2d/blend2d.h>

#include <proj/metadata/primitiveSpec.h>
#include <proj/primitives/frame.h>
#include <proj/primitives/realtime.h>

namespace primitives {

/// A rendering context bound to one frame's pixels. Built with the frame
/// (on the builder thread); drawing reuses it, so nothing is set up per
/// frame on the render thread.
class canvas {
public:
    /// Binds a context to @p f's pixels. Throws std::runtime_error when
    /// Blend2D cannot.
    explicit canvas(frame& f);
    ~canvas();

    canvas(const canvas&) = delete;
    canvas& operator=(const canvas&) = delete;
    canvas(canvas&&) = delete;
    canvas& operator=(canvas&&) = delete;

    /// Composites @p color over every pixel (source over).
    void fillAll(const metadata::rgba& color) noexcept PROJ_NONBLOCKING;

private:
    BLImage image_;
    BLContext context_;
};

/// @p color as a Blend2D color.
BLRgba toBlend(const metadata::rgba& color) noexcept PROJ_NONBLOCKING;

} // namespace primitives
