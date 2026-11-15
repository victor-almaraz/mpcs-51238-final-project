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
}

canvas::~canvas() { context_.end(); }

BLRgba toBlend(const metadata::rgba& color) noexcept PROJ_NONBLOCKING {
    return {static_cast<float>(color[0]), static_cast<float>(color[1]), static_cast<float>(color[2]),
            static_cast<float>(color[3])};
}

void canvas::fillAll(const metadata::rgba& color) noexcept PROJ_NONBLOCKING {
    context_.fill_all(toBlend(color));
    context_.flush(BL_CONTEXT_FLUSH_SYNC);
}

} // namespace primitives
