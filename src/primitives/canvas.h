// Drawing into a frame with Blend2D (primitives.md §7.7). Internal to
// primitives, which links Blend2D privately: no Blend2D type appears in a
// public header.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <proj/metadata/primitiveSpec.h>
#include <proj/primitives/frame.h>
#include <proj/primitives/realtime.h>

#include <blend2d/blend2d.h>

namespace primitives {

/// How a stroke ends, with Processing's names: `round`, `square` (flat at
/// the end point), `project` (flat, half the weight past it). In the
/// order of the `stroke-cap` choices (primitiveSpec.cpp).
enum class cap : std::uint8_t { round, square, project };

/// How a shape is painted: fill, then stroke (§7.7.3). An absent color is
/// `none`.
struct paint {
    /// The fill color, or none.
    std::optional<metadata::rgba> fill;
    /// The stroke color, or none.
    std::optional<metadata::rgba> stroke;
    /// Stroke width, in pixels; 0 draws no stroke.
    double weight = 1.0;
    /// How open strokes end.
    cap ends = cap::round;
};

/// A point, in pixels.
struct vertex {
    double x = 0.0;
    double y = 0.0;
};

/// Most vertices of a polygon the canvas draws (a quad has four).
inline constexpr std::size_t maxPolygon = 8;

/// How an arc closes (Processing's arc modes): `open` fills the chord
/// region and strokes only the curve, `chord` closes with a straight line,
/// `pie` with two radii. In the order of arc's `mode` choices.
enum class arcMode : std::uint8_t { open, chord, pie };

/// A rectangle by its top-left corner and size, in pixels.
struct box {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

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

    /// Paints rectangle @p r through transform @p t.
    void rect(const box& r, const paint& p, const affine& t) noexcept PROJ_NONBLOCKING;
    /// Paints the ellipse inscribed in @p bounds through transform @p t.
    void ellipse(const box& bounds, const paint& p, const affine& t) noexcept PROJ_NONBLOCKING;
    /// Strokes the segment from (@p x1, @p y1) to (@p x2, @p y2).
    void line(double x1, double y1, double x2, double y2, const paint& p,
              const affine& t) noexcept PROJ_NONBLOCKING;
    /// Paints the closed polygon through @p points, at most maxPolygon of
    /// them (more are ignored).
    void polygon(std::span<const vertex> points, const paint& p, const affine& t) noexcept PROJ_NONBLOCKING;
    /// Paints the arc of the ellipse inscribed in @p bounds from @p start
    /// to @p stop radians, clockwise from the positive x axis.
    void arc(const box& bounds, double start, double stop, arcMode mode, const paint& p,
             const affine& t) noexcept PROJ_NONBLOCKING;
    /// Strokes the cubic Bezier curve from @p from through controls @p c1
    /// and @p c2 to @p to.
    void bezier(vertex from, vertex c1, vertex c2, vertex to, const paint& p,
                const affine& t) noexcept PROJ_NONBLOCKING;
    /// Draws @p source's pixels scaled into @p dest, through @p t.
    void blit(const canvas& source, const box& dest, const affine& t) noexcept PROJ_NONBLOCKING;
    /// Draws a point at (@p x, @p y): a dot of the stroke color and weight,
    /// round for a round cap and square otherwise.
    void point(double x, double y, const paint& p, const affine& t) noexcept PROJ_NONBLOCKING;

private:
    // Paints Blend2D geometry @p shape, of @p type, through @p t.
    template <typename Shape>
    void geometry(BLGeometryType type, const Shape& shape, const paint& p,
                  const affine& t) noexcept PROJ_NONBLOCKING;

    // Draws once into the frame on the builder thread, so the context's
    // work paths and edge storage hold their capacity before the render
    // thread draws (the first stroke would allocate them).
    void warmUp(int width, int height);

    BLImage image_;
    BLContext context_;
    // Built anew for each curve; capacity reserved at construction, kept
    // across clear(), so building it does not allocate.
    BLPath curve_;
};

/// @p color as a Blend2D color.
BLRgba toBlend(const metadata::rgba& color) noexcept PROJ_NONBLOCKING;

}  // namespace primitives
