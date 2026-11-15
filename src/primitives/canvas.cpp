#include "canvas.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

#include <proj/metadata/primitiveSpec.h>
#include <proj/primitives/frame.h>
#include <proj/primitives/realtime.h>

#include <blend2d/blend2d.h>

namespace primitives {

namespace {

void check(BLResult result, const char* what) {
    if (result != BL_SUCCESS) throw std::runtime_error(std::string("Blend2D: ") + what + " failed");
}

// Vertices a curve path holds without growing: a cubic is four.
constexpr std::size_t curveVertices = 16;

// Processing's cap names in Blend2D's terms.
BLStrokeCap strokeCap(cap c) noexcept PROJ_NONBLOCKING {
    switch (c) {
    case cap::square: return BL_STROKE_CAP_BUTT;
    case cap::project: return BL_STROKE_CAP_SQUARE;
    case cap::round: break;
    }
    return BL_STROKE_CAP_ROUND;
}

} // namespace

canvas::canvas(frame& f) {
    // Blend2D takes external pixel storage by pointer and stride: the
    // narrow unsafe use at this library boundary (modern-cpp skill,
    // section 9). The frame owns the storage, which never moves.
    const auto stride = static_cast<std::intptr_t>(f.width()) * static_cast<std::intptr_t>(sizeof(std::uint32_t));
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
    context_.flush(BL_CONTEXT_FLUSH_SYNC);
}

canvas::~canvas() { context_.end(); }

BLRgba toBlend(const metadata::rgba& color) noexcept PROJ_NONBLOCKING {
    return {static_cast<float>(color[0]), static_cast<float>(color[1]), static_cast<float>(color[2]),
            static_cast<float>(color[3])};
}

// Blend2D takes geometry by type and untyped pointer: the narrow unsafe
// use at this library boundary (modern-cpp skill, section 9).
void canvas::geometry(BLGeometryType type, const void* data, const paint& p,
                      const affine& t) noexcept PROJ_NONBLOCKING {
    context_.set_transform(BLMatrix2D(t.xx, t.yx, t.xy, t.yy, t.x0, t.y0));
    if (p.fill) context_.fill_geometry(type, data, toBlend(*p.fill));
    if (p.stroke && p.weight > 0.0) {
        context_.set_stroke_width(p.weight);
        context_.set_stroke_caps(strokeCap(p.ends));
        context_.stroke_geometry(type, data, toBlend(*p.stroke));
    }
    context_.reset_transform();
    context_.flush(BL_CONTEXT_FLUSH_SYNC);
}

void canvas::rect(const box& r, const paint& p, const affine& t) noexcept PROJ_NONBLOCKING {
    const BLRect shape(r.x, r.y, r.width, r.height);
    geometry(BL_GEOMETRY_TYPE_RECTD, &shape, p, t);
}

void canvas::ellipse(const box& bounds, const paint& p, const affine& t) noexcept PROJ_NONBLOCKING {
    const double rx = bounds.width / 2.0;
    const double ry = bounds.height / 2.0;
    const BLEllipse shape(bounds.x + rx, bounds.y + ry, rx, ry);
    geometry(BL_GEOMETRY_TYPE_ELLIPSE, &shape, p, t);
}

void canvas::line(double x1, double y1, double x2, double y2, const paint& p,
                  const affine& t) noexcept PROJ_NONBLOCKING {
    const BLLine shape(x1, y1, x2, y2);
    geometry(BL_GEOMETRY_TYPE_LINE, &shape,
             {.fill = std::nullopt, .stroke = p.stroke, .weight = p.weight, .ends = p.ends}, t);
}

void canvas::polygon(std::span<const vertex> points, const paint& p,
                     const affine& t) noexcept PROJ_NONBLOCKING {
    std::array<BLPoint, maxPolygon> corners{};
    const std::size_t n = std::min(points.size(), corners.size());
    for (std::size_t i = 0; i < n; ++i) corners[i] = BLPoint(points[i].x, points[i].y);
    const BLArrayView<BLPoint> shape{.data = corners.data(), .size = n};
    geometry(BL_GEOMETRY_TYPE_POLYGOND, &shape, p, t);
}

void canvas::arc(const box& bounds, double start, double stop, arcMode mode, const paint& p,
                 const affine& t) noexcept PROJ_NONBLOCKING {
    const double rx = bounds.width / 2.0;
    const double ry = bounds.height / 2.0;
    const BLArc shape(bounds.x + rx, bounds.y + ry, rx, ry, start, stop - start);
    const paint fillOnly{.fill = p.fill, .stroke = std::nullopt, .weight = 0.0, .ends = p.ends};
    const paint strokeOnly{.fill = std::nullopt, .stroke = p.stroke, .weight = p.weight, .ends = p.ends};
    switch (mode) {
    case arcMode::open:
        geometry(BL_GEOMETRY_TYPE_CHORD, &shape, fillOnly, t);
        geometry(BL_GEOMETRY_TYPE_ARC, &shape, strokeOnly, t);
        break;
    case arcMode::chord: geometry(BL_GEOMETRY_TYPE_CHORD, &shape, p, t); break;
    case arcMode::pie: geometry(BL_GEOMETRY_TYPE_PIE, &shape, p, t); break;
    }
}

void canvas::bezier(vertex from, vertex c1, vertex c2, vertex to, const paint& p,
                    const affine& t) noexcept PROJ_NONBLOCKING {
    curve_.clear();
    curve_.move_to(from.x, from.y);
    curve_.cubic_to(c1.x, c1.y, c2.x, c2.y, to.x, to.y);
    const paint strokeOnly{.fill = std::nullopt, .stroke = p.stroke, .weight = p.weight, .ends = p.ends};
    geometry(BL_GEOMETRY_TYPE_PATH, &curve_, strokeOnly, t);
}

void canvas::blit(const canvas& source, const box& dest, const affine& t) noexcept PROJ_NONBLOCKING {
    context_.set_transform(BLMatrix2D(t.xx, t.yx, t.xy, t.yy, t.x0, t.y0));
    context_.blit_image(BLRect(dest.x, dest.y, dest.width, dest.height), source.image_);
    context_.reset_transform();
    context_.flush(BL_CONTEXT_FLUSH_SYNC);
}

void canvas::point(double x, double y, const paint& p, const affine& t) noexcept PROJ_NONBLOCKING {
    const double r = p.weight / 2.0;
    const paint dot{.fill = p.stroke, .stroke = std::nullopt, .weight = 0.0, .ends = p.ends};
    if (p.ends == cap::round) {
        ellipse({.x = x - r, .y = y - r, .width = p.weight, .height = p.weight}, dot, t);
    } else {
        rect({.x = x - r, .y = y - r, .width = p.weight, .height = p.weight}, dot, t);
    }
}

void canvas::fillAll(const metadata::rgba& color) noexcept PROJ_NONBLOCKING {
    context_.fill_all(toBlend(color));
    context_.flush(BL_CONTEXT_FLUSH_SYNC);
}

} // namespace primitives
