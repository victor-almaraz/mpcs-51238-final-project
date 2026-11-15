#include "canvas.h"

#include <blend2d/blend2d.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include <proj/metadata/primitiveSpec.h>
#include <proj/primitives/frame.h>
#include <proj/primitives/realtime.h>

namespace primitives {

namespace {

void check(BLResult result, const char* what) {
    if (result != BL_SUCCESS) throw std::runtime_error(std::string("Blend2D: ") + what + " failed");
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

void canvas::fillAll(const metadata::rgba& color) noexcept PROJ_NONBLOCKING {
    context_.fill_all(toBlend(color));
    context_.flush(BL_CONTEXT_FLUSH_SYNC);
}

} // namespace primitives
