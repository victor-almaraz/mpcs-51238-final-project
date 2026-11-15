// Image generators and the visual layer (primitives.md §7.7, §7.3). They
// run on the render thread, once per frame: a generator starts from its
// input frame, or a transparent one, and draws over it into its output
// (§5.5.4).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>

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

// The stroke of a point or a line: color, weight, and cap; no fill.
paint strokeOf(const nodeContext& c) {
    return {.fill = std::nullopt,
            .stroke = c.color("stroke"),
            .weight = c.number("stroke-weight"),
            .ends = static_cast<cap>(c.choice("stroke-cap"))};
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

// A shape drawn from fixed points: point, line.
class pointShape final : public nodeBase {
public:
    explicit pointShape(nodeContext& c)
        : nodeBase(c),
          in_(c.videoIn("in")),
          out_(c.videoOut("out")),
          x_(c.number("x")),
          y_(c.number("y")),
          paint_(strokeOf(c)) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        startFrom(*out_, in_);
        frame& f = out_->image();
        f.drawing().point(x_, y_, paint_, f.transform());
    }

private:
    std::shared_ptr<const vPipe> in_;
    std::shared_ptr<vPipe> out_;
    double x_;
    double y_;
    paint paint_;
};

class lineShape final : public nodeBase {
public:
    explicit lineShape(nodeContext& c)
        : nodeBase(c),
          in_(c.videoIn("in")),
          out_(c.videoOut("out")),
          ends_{c.number("x1"), c.number("y1"), c.number("x2"), c.number("y2")},
          paint_(strokeOf(c)) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        startFrom(*out_, in_);
        frame& f = out_->image();
        f.drawing().line(ends_[0], ends_[1], ends_[2], ends_[3], paint_, f.transform());
    }

private:
    std::shared_ptr<const vPipe> in_;
    std::shared_ptr<vPipe> out_;
    std::array<double, 4> ends_;
    paint paint_;
};

// A closed polygon through N points named x1, y1 to xN, yN: triangle (3),
// quad (4).
template <std::size_t N>
class polygonShape final : public nodeBase {
public:
    explicit polygonShape(nodeContext& c)
        : nodeBase(c), in_(c.videoIn("in")), out_(c.videoOut("out")), paint_(paintOf(c)) {
        for (std::size_t i = 0; i < N; ++i) {
            const std::string k = std::to_string(i + 1);
            corners_[i] = {.x = c.number("x" + k), .y = c.number("y" + k)};
        }
    }

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        startFrom(*out_, in_);
        frame& f = out_->image();
        f.drawing().polygon(corners_, paint_, f.transform());
    }

private:
    std::shared_ptr<const vPipe> in_;
    std::shared_ptr<vPipe> out_;
    std::array<vertex, N> corners_{};
    paint paint_;
};

class arcShape final : public nodeBase {
public:
    explicit arcShape(nodeContext& c)
        : nodeBase(c),
          in_(c.videoIn("in")),
          out_(c.videoOut("out")),
          bounds_(placed(2, c.number("x"), c.number("y"), c.number("width"), c.number("height"))),
          start_(c.number("start")),
          stop_(c.number("stop")),
          mode_(static_cast<arcMode>(c.choice("mode"))),
          paint_(paintOf(c)) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        startFrom(*out_, in_);
        frame& f = out_->image();
        f.drawing().arc(bounds_, start_, stop_, mode_, paint_, f.transform());
    }

private:
    std::shared_ptr<const vPipe> in_;
    std::shared_ptr<vPipe> out_;
    box bounds_;
    double start_;
    double stop_;
    arcMode mode_;
    paint paint_;
};

class bezierShape final : public nodeBase {
public:
    explicit bezierShape(nodeContext& c)
        : nodeBase(c),
          in_(c.videoIn("in")),
          out_(c.videoOut("out")),
          points_{vertex{.x = c.number("x1"), .y = c.number("y1")},
                  vertex{.x = c.number("cx1"), .y = c.number("cy1")},
                  vertex{.x = c.number("cx2"), .y = c.number("cy2")},
                  vertex{.x = c.number("x2"), .y = c.number("y2")}},
          paint_{.fill = std::nullopt, .stroke = c.color("stroke"), .weight = c.number("stroke-weight")} {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        startFrom(*out_, in_);
        frame& f = out_->image();
        f.drawing().bezier(points_[0], points_[1], points_[2], points_[3], paint_, f.transform());
    }

private:
    std::shared_ptr<const vPipe> in_;
    std::shared_ptr<vPipe> out_;
    std::array<vertex, 4> points_;
    paint paint_;
};

// Processing's translate, rotate, then scale as one matrix: a point is
// scaled, rotated (clockwise, since y points down), then moved.
affine composed(double tx, double ty, double angle, double sx, double sy) noexcept {
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    return {.xx = c * sx, .yx = s * sx, .xy = -s * sy, .yy = c * sy, .x0 = tx, .y0 = ty};
}

// Sets the transform that drawing downstream applies, until the next
// transform: it replaces Sketching's matrix stack (§7.7.2).
class transform final : public nodeBase {
public:
    explicit transform(nodeContext& c)
        : nodeBase(c),
          in_(c.videoIn("in")),
          out_(c.videoOut("out")),
          matrix_(composed(c.number("translate-x"), c.number("translate-y"), c.number("rotate"),
                           c.number("scale-x"), c.number("scale-y"))) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        startFrom(*out_, in_);
        out_->image().setTransform(matrix_);
    }

private:
    std::shared_ptr<const vPipe> in_;
    std::shared_ptr<vPipe> out_;
    affine matrix_;
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

// A color between two colors, per block, to the four channels (R, G, B,
// A) of a control pipe that image parameters follow (§7.7). It draws
// nothing, so it runs on the audio thread like any control generator.
class lerpColor final : public nodeBase {
public:
    explicit lerpColor(nodeContext& c)
        : nodeBase(c),
          a_(c.color("color-a").value_or(metadata::rgba{})),
          b_(c.color("color-b").value_or(metadata::rgba{})),
          amount_(c.param("amount")),
          out_(c.controlOut("out")) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        if (!out_.pipe) return;
        const double t = std::clamp(static_cast<double>(amount_.value()), 0.0, 1.0);
        for (int ch = 0; ch < std::min(out_.channels, metadata::colorChannels); ++ch) {
            const auto i = static_cast<std::size_t>(ch);
            emit(*out_.pipe, ch, static_cast<float>(a_[i] + ((b_[i] - a_[i]) * t)));
        }
    }

private:
    metadata::rgba a_;
    metadata::rgba b_;
    signal amount_;
    controlOutput out_;
};

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
        {.type = "point", .make = make<pointShape>},
        {.type = "line", .make = make<lineShape>},
        {.type = "rect", .make = makeRect},
        {.type = "triangle", .make = make<polygonShape<3>>},
        {.type = "quad", .make = make<polygonShape<4>>},
        {.type = "arc", .make = make<arcShape>},
        {.type = "bezier", .make = make<bezierShape>},
        {.type = "transform", .make = make<transform>},
        {.type = "lerpColor", .make = make<lerpColor>},
        {.type = "ellipse", .make = makeEllipse},
        {.type = "circle", .make = makeCircle},
        {.type = "videoOut", .make = make<videoOut>},
    });
    return factories;
}

} // namespace primitives
