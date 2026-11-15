// Image generators and the visual layer (primitives.md §7.7, §7.3). They
// run on the render thread, once per frame: a generator starts from its
// input frame, or a transparent one, and draws over it into its output
// (§5.5.4).

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <span>

#include <proj/metadata/primitiveSpec.h>
#include <proj/primitives/frame.h>
#include <proj/primitives/pipe.h>
#include <proj/primitives/primitive.h>
#include <proj/primitives/realtime.h>

#include "canvas.h"
#include "node.h"

namespace primitives {

namespace {

// Starts @p out's frame from @p in, or from a transparent frame.
void startFrom(vPipe& out, const std::shared_ptr<const vPipe>& in) noexcept PROJ_NONBLOCKING {
    if (in) {
        out.image().copyFrom(in->image());
    } else {
        out.image().clear();
    }
}

// The input frame with a color composited over it: an opaque color
// replaces it, a translucent one fades it.
class background final : public nodeBase {
public:
    explicit background(nodeContext& c)
        : nodeBase(c), in_(c.videoIn("in")), out_(c.videoOut("out")),
          color_(c.color("color").value_or(metadata::rgba{0.0, 0.0, 0.0, 1.0})) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        startFrom(*out_, in_);
        out_->image().fill(color_);
    }

private:
    std::shared_ptr<const vPipe> in_;
    std::shared_ptr<vPipe> out_;
    metadata::rgba color_;
};

// Fill, stroke, and stroke weight, as most shapes take them (§7.7.3).
paint paintOf(const nodeContext& c) {
    return {.fill = c.color("fill"), .stroke = c.color("stroke"), .weight = c.number("stroke-weight")};
}

// The rectangle of Processing's rectMode: corner (x, y, width, height),
// corners (two opposite corners), center, or radius (half sizes about the
// center).
box placed(int mode, double x, double y, double w, double h) noexcept {
    switch (mode) {
    case 1:
        return {
            .x = std::min(x, w), .y = std::min(y, h), .width = std::abs(w - x), .height = std::abs(h - y)};
    case 2: return {.x = x - (w / 2.0), .y = y - (h / 2.0), .width = w, .height = h};
    case 3: return {.x = x - w, .y = y - h, .width = 2.0 * w, .height = 2.0 * h};
    default: return {.x = x, .y = y, .width = w, .height = h};
    }
}

// The bounds of Processing's ellipseMode: center (x, y, width, height),
// radius (half sizes about the center), corner, or corners.
box ellipseBounds(int mode, double x, double y, double w, double h) noexcept {
    switch (mode) {
    case 1: return placed(3, x, y, w, h);
    case 2: return placed(0, x, y, w, h);
    case 3: return placed(1, x, y, w, h);
    default: return placed(2, x, y, w, h);
    }
}

// How a boxed shape draws itself: a stateless drawer per shape.
struct drawRect {
    static void draw(canvas& c, const box& b, const paint& p, const affine& t) noexcept PROJ_NONBLOCKING {
        c.rect(b, p, t);
    }
};
struct drawEllipse {
    static void draw(canvas& c, const box& b, const paint& p, const affine& t) noexcept PROJ_NONBLOCKING {
        c.ellipse(b, p, t);
    }
};

// A shape drawn in fixed bounds: rect, ellipse, circle.
template <typename Draw>
class boxed final : public nodeBase {
public:
    boxed(nodeContext& c, box bounds)
        : nodeBase(c), in_(c.videoIn("in")), out_(c.videoOut("out")), bounds_(bounds), paint_(paintOf(c)) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        startFrom(*out_, in_);
        frame& f = out_->image();
        Draw::draw(f.drawing(), bounds_, paint_, f.transform());
    }

private:
    std::shared_ptr<const vPipe> in_;
    std::shared_ptr<vPipe> out_;
    box bounds_;
    paint paint_;
};

std::shared_ptr<primitive> makeRect(nodeContext& c) {
    return std::make_shared<boxed<drawRect>>(
        c, placed(c.choice("mode"), c.number("x"), c.number("y"), c.number("width"), c.number("height")));
}

std::shared_ptr<primitive> makeEllipse(nodeContext& c) {
    return std::make_shared<boxed<drawEllipse>>(
        c,
        ellipseBounds(c.choice("mode"), c.number("x"), c.number("y"), c.number("width"), c.number("height")));
}

std::shared_ptr<primitive> makeCircle(nodeContext& c) {
    const double d = c.number("diameter");
    return std::make_shared<boxed<drawEllipse>>(c, placed(2, c.number("x"), c.number("y"), d, d));
}

// The visual layer (v1 has one): the server shows its input frame. It
// draws nothing; the plan records the frame it shows.
class videoOut final : public nodeBase {
public:
    explicit videoOut(nodeContext& c) : nodeBase(c) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {}
};

} // namespace

std::span<const factoryEntry> imageFactories() {
    static const auto factories = std::to_array<factoryEntry>({
        {.type = "background", .make = make<background>},
        {.type = "rect", .make = makeRect},
        {.type = "ellipse", .make = makeEllipse},
        {.type = "circle", .make = makeCircle},
        {.type = "videoOut", .make = make<videoOut>},
    });
    return factories;
}

} // namespace primitives
