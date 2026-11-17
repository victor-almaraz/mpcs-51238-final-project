#include "canvas.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include <espresso/metadata/primitiveSpec.h>
#include <espresso/primitives/frame.h>
#include <espresso/primitives/realtime.h>

#include <blend2d/blend2d.h>

namespace primitives {

namespace {

void check(BLResult result, std::string_view what) {
    if (result != BL_SUCCESS) throw std::runtime_error("Blend2D: " + std::string(what) + " failed");
}

// Vertices a curve path holds without growing: a cubic is four.
constexpr std::size_t curveVertices = 16;

// Processing's cap names in Blend2D's terms.
BLStrokeCap strokeCap(cap c) noexcept ESPRESSO_NONBLOCKING {
    switch (c) {
    case cap::square: return BL_STROKE_CAP_BUTT;
    case cap::project: return BL_STROKE_CAP_SQUARE;
    case cap::round: break;
    }
    return BL_STROKE_CAP_ROUND;
}

}  // namespace

canvas::canvas(frame& f) {
    // Blend2D takes external pixel storage by pointer and stride: the
    // narrow unsafe use at this library boundary (modern-cpp skill,
    // section 9). The frame owns the storage, which never moves.
    const auto stride =
        static_cast<std::intptr_t>(f.width()) * static_cast<std::intptr_t>(sizeof(std::uint32_t));
    check(image_.create_from_data(f.width(), f.height(), BL_FORMAT_PRGB32, f.pixels().data(), stride),
          "binding frame pixels");
    check(context_.begin(image_), "starting a context");
    warmUp(f.width(), f.height());
}

void canvas::warmUp(int width, int height) {
    // Not through the nonblocking drawing functions: this runs on the
    // builder thread, where allocation is allowed.
    const double w = width;
    const double h = height;
    const BLRgba white(1.0f, 1.0f, 1.0f, 1.0f);
    const BLRect whole(0.0, 0.0, w, h);
    const BLEllipse oval(w / 2.0, h / 2.0, w / 2.0, h / 2.0);
    context_.set_stroke_width(8.0);
    context_.fill_rect(whole, white);
    context_.stroke_rect(whole, white);
    context_.fill_ellipse(oval, white);
    context_.stroke_ellipse(oval, white);
    // Open figures take another branch of the stroker: a line with caps,
    // and a curve.
    context_.set_stroke_caps(BL_STROKE_CAP_ROUND);
    context_.stroke_line(BLLine(0.0, 0.0, w, h), white);
    check(curve_.reserve(curveVertices), "reserving a path");
    curve_.move_to(0.0, h);
    curve_.cubic_to(0.0, 0.0, w, h, w, 0.0);
    context_.stroke_path(curve_, white);
    curve_.clear();
    // The warm-up leaves no mark: the frame starts transparent.
    context_.clear_all();
}

canvas::~canvas() { context_.end(); }

// The drawing functions call Blend2D, which has no nonblocking
// annotations. The canvas warms its context and reserves curve_ when it is
// built (warmUp), so drawing reuses that storage; the RealtimeSanitizer
// tests of every image generator check it at run time.
ESPRESSO_ASSUME_NONBLOCKING_BEGIN

namespace {

BLRgba toBlend(const metadata::rgba& color) noexcept ESPRESSO_NONBLOCKING {
    return {static_cast<float>(color[0]), static_cast<float>(color[1]), static_cast<float>(color[2]),
            static_cast<float>(color[3])};
}

BLMatrix2D toBlend(const affine& t) noexcept ESPRESSO_NONBLOCKING {
    return {t.xx, t.yx, t.xy, t.yy, t.x0, t.y0};
}

// @p p without its fill, or without its stroke.
paint strokeOnly(const paint& p) noexcept ESPRESSO_NONBLOCKING {
    return {.fill = std::nullopt, .stroke = p.stroke, .weight = p.weight, .ends = p.ends};
}
paint fillOnly(const paint& p) noexcept ESPRESSO_NONBLOCKING {
    return {.fill = p.fill, .stroke = std::nullopt, .weight = 0.0, .ends = p.ends};
}

// The center and radii of the ellipse inscribed in a box.
struct oval {
    double cx = 0.0;
    double cy = 0.0;
    double rx = 0.0;
    double ry = 0.0;
};
oval inscribed(const box& b) noexcept ESPRESSO_NONBLOCKING {
    const double rx = b.width / 2.0;
    const double ry = b.height / 2.0;
    return {.cx = b.x + rx, .cy = b.y + ry, .rx = rx, .ry = ry};
}

}  // namespace

// The context is synchronous (begin() without threads), so each call
// draws at once: no flush. Every draw sets the transform it uses.

// Blend2D takes geometry by type and untyped pointer: the narrow unsafe
// use at this library boundary (modern-cpp skill, section 9), confined to
// the two calls here.
template <typename Shape>
void canvas::geometry(BLGeometryType type, const Shape& shape, const paint& p,
                      const affine& t) noexcept ESPRESSO_NONBLOCKING {
    context_.set_transform(toBlend(t));
    if (p.fill) context_.fill_geometry(type, &shape, toBlend(*p.fill));
    if (p.stroke && p.weight > 0.0) {
        context_.set_stroke_width(p.weight);
        context_.set_stroke_caps(strokeCap(p.ends));
        context_.stroke_geometry(type, &shape, toBlend(*p.stroke));
    }
}

void canvas::rect(const box& r, const paint& p, const affine& t) noexcept ESPRESSO_NONBLOCKING {
    geometry(BL_GEOMETRY_TYPE_RECTD, BLRect(r.x, r.y, r.width, r.height), p, t);
}

void canvas::ellipse(const box& bounds, const paint& p, const affine& t) noexcept ESPRESSO_NONBLOCKING {
    const oval o = inscribed(bounds);
    geometry(BL_GEOMETRY_TYPE_ELLIPSE, BLEllipse(o.cx, o.cy, o.rx, o.ry), p, t);
}

void canvas::line(double x1, double y1, double x2, double y2, const paint& p,
                  const affine& t) noexcept ESPRESSO_NONBLOCKING {
    geometry(BL_GEOMETRY_TYPE_LINE, BLLine(x1, y1, x2, y2), strokeOnly(p), t);
}

void canvas::polygon(std::span<const vertex> points, const paint& p,
                     const affine& t) noexcept ESPRESSO_NONBLOCKING {
    std::array<BLPoint, maxPolygon> corners{};
    const std::size_t n = std::min(points.size(), corners.size());
    for (std::size_t i = 0; i < n; ++i) corners[i] = BLPoint(points[i].x, points[i].y);
    geometry(BL_GEOMETRY_TYPE_POLYGOND, BLArrayView<BLPoint>{.data = corners.data(), .size = n}, p, t);
}

void canvas::arc(const box& bounds, double start, double stop, arcMode mode, const paint& p,
                 const affine& t) noexcept ESPRESSO_NONBLOCKING {
    const oval o = inscribed(bounds);
    const BLArc shape(o.cx, o.cy, o.rx, o.ry, start, stop - start);
    switch (mode) {
    case arcMode::open:
        geometry(BL_GEOMETRY_TYPE_CHORD, shape, fillOnly(p), t);
        geometry(BL_GEOMETRY_TYPE_ARC, shape, strokeOnly(p), t);
        break;
    case arcMode::chord: geometry(BL_GEOMETRY_TYPE_CHORD, shape, p, t); break;
    case arcMode::pie: geometry(BL_GEOMETRY_TYPE_PIE, shape, p, t); break;
    }
}

void canvas::bezier(vertex from, vertex c1, vertex c2, vertex to, const paint& p,
                    const affine& t) noexcept ESPRESSO_NONBLOCKING {
    curve_.clear();
    curve_.move_to(from.x, from.y);
    curve_.cubic_to(c1.x, c1.y, c2.x, c2.y, to.x, to.y);
    geometry(BL_GEOMETRY_TYPE_PATH, curve_, strokeOnly(p), t);
}

void canvas::blit(const canvas& source, const box& dest, const affine& t) noexcept ESPRESSO_NONBLOCKING {
    context_.set_transform(toBlend(t));
    context_.blit_image(BLRect(dest.x, dest.y, dest.width, dest.height), source.image_);
}

void canvas::point(double x, double y, const paint& p, const affine& t) noexcept ESPRESSO_NONBLOCKING {
    const double r = p.weight / 2.0;
    const box dot{.x = x - r, .y = y - r, .width = p.weight, .height = p.weight};
    const paint ink{.fill = p.stroke, .stroke = std::nullopt, .weight = 0.0, .ends = p.ends};
    if (p.ends == cap::round) {
        ellipse(dot, ink, t);
    } else {
        rect(dot, ink, t);
    }
}

// fill_all covers the whole image whatever the transform.
void canvas::fillAll(const metadata::rgba& color) noexcept ESPRESSO_NONBLOCKING {
    context_.fill_all(toBlend(color));
}

ESPRESSO_ASSUME_NONBLOCKING_END

}  // namespace primitives
