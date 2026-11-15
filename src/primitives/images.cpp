// Image generators and the visual layer (primitives.md §7.7, §7.3). They
// run on the render thread, once per frame: a generator starts from its
// input frame, or a transparent one, and draws over it into its output
// (§5.5.4); in a drawing chain, the builder binds both to one frame
// (§5.5.5). Geometry is read each frame, so it can follow pipes.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <proj/metadata/primitiveSpec.h>
#include <proj/primitives/frame.h>
#include <proj/primitives/pipe.h>
#include <proj/primitives/primitive.h>
#include <proj/primitives/realtime.h>

#include "canvas.h"
#include "node.h"

namespace primitives {

namespace {

// A video node that draws over its input frame into its output (§7.7.1).
class drawing : public nodeBase {
protected:
    explicit drawing(nodeContext& c) : nodeBase(c), in_(c.videoIn("in")), out_(c.videoOut("out")) {}

    // Starts this frame's output from the input frame, or a transparent
    // one, and returns it. Inside a drawing chain the input is the output
    // (§5.5.5): the frame already holds what the node before drew.
    frame& begin() noexcept PROJ_NONBLOCKING {
        frame& f = out_->image();
        if (!in_) {
            f.clear();
        } else if (in_ != out_) {
            f.copyFrom(in_->image());
        }
        return f;
    }

private:
    std::shared_ptr<const vPipe> in_;
    std::shared_ptr<vPipe> out_;
};

// The input frame with a color composited over it: an opaque color
// replaces it, a translucent one fades it.
class background final : public drawing {
public:
    explicit background(nodeContext& c) : drawing(c), color_(c.colorParam("color")) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        frame& f = begin();
        if (const auto color = color_.value()) f.fill(*color);
    }

private:
    frameColor color_;
};

// What a shape paints with, read each frame (§7.7.3): fill (for shapes
// that have one), stroke, stroke weight, and cap (for points and lines).
class paintSource {
public:
    enum class parts : std::uint8_t { fillAndStroke, strokeAndCap, strokeOnly };

    paintSource(nodeContext& c, parts which)
        : fill_(which == parts::fillAndStroke ? c.colorParam("fill") : frameColor()),
          stroke_(c.colorParam("stroke")),
          weight_(c.frameParam("stroke-weight")),
          ends_(which == parts::strokeAndCap ? static_cast<cap>(c.choice("stroke-cap")) : cap::round) {}

    paint now() const noexcept {
        return {.fill = fill_.value(), .stroke = stroke_.value(), .weight = weight_.value(), .ends = ends_};
    }

private:
    frameColor fill_;
    frameColor stroke_;
    frameValue weight_;
    cap ends_ = cap::round;
};

// How four numbers place a box: Processing's rectMode and ellipseMode.
// corner: x, y, width, height; corners: two opposite corners; center:
// the center and the size; radius: the center and half sizes.
enum class placement : std::uint8_t { corner, corners, center, radius };

// The placements of rect's and ellipse's mode choices, in the order of
// their lists in primitiveSpec.cpp (the mode tests name each one).
constexpr std::array rectModes{placement::corner, placement::corners, placement::center, placement::radius};
constexpr std::array ellipseModes{placement::center, placement::radius, placement::corner,
                                  placement::corners};

box placed(placement mode, double x, double y, double w, double h) noexcept {
    switch (mode) {
    case placement::corners:
        return {
            .x = std::min(x, w), .y = std::min(y, h), .width = std::abs(w - x), .height = std::abs(h - y)};
    case placement::center: return {.x = x - (w / 2.0), .y = y - (h / 2.0), .width = w, .height = h};
    case placement::radius: return {.x = x - w, .y = y - h, .width = 2.0 * w, .height = 2.0 * h};
    case placement::corner: break;
    }
    return {.x = x, .y = y, .width = w, .height = h};
}

// A shape drawn in bounds it reads each frame: rect, or an ellipse (ellipse,
// circle).
class boxed final : public drawing {
public:
    // @p width and @p height name the parameters for the size; a circle
    // names its diameter for both.
    boxed(nodeContext& c, bool oval, std::string_view width, std::string_view height, placement mode)
        : drawing(c),
          oval_(oval),
          x_(c.frameParam("x")),
          y_(c.frameParam("y")),
          w_(c.frameParam(width)),
          h_(c.frameParam(height)),
          mode_(mode),
          paint_(c, paintSource::parts::fillAndStroke) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        frame& f = begin();
        const box bounds = placed(mode_, x_.value(), y_.value(), w_.value(), h_.value());
        if (oval_) {
            f.drawing().ellipse(bounds, paint_.now(), f.transform());
        } else {
            f.drawing().rect(bounds, paint_.now(), f.transform());
        }
    }

private:
    bool oval_ = false;
    frameValue x_;
    frameValue y_;
    frameValue w_;
    frameValue h_;
    placement mode_ = placement::corner;
    paintSource paint_;
};

class pointShape final : public drawing {
public:
    explicit pointShape(nodeContext& c)
        : drawing(c),
          x_(c.frameParam("x")),
          y_(c.frameParam("y")),
          paint_(c, paintSource::parts::strokeAndCap) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        frame& f = begin();
        f.drawing().point(x_.value(), y_.value(), paint_.now(), f.transform());
    }

private:
    frameValue x_;
    frameValue y_;
    paintSource paint_;
};

class lineShape final : public drawing {
public:
    explicit lineShape(nodeContext& c)
        : drawing(c),
          ends_{c.frameParam("x1"), c.frameParam("y1"), c.frameParam("x2"), c.frameParam("y2")},
          paint_(c, paintSource::parts::strokeAndCap) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        frame& f = begin();
        f.drawing().line(ends_[0].value(), ends_[1].value(), ends_[2].value(), ends_[3].value(), paint_.now(),
                         f.transform());
    }

private:
    std::array<frameValue, 4> ends_;
    paintSource paint_;
};

// A closed polygon through N points named x1, y1 to xN, yN: triangle (3),
// quad (4).
template <std::size_t N>
class polygonShape final : public drawing {
    static_assert(N <= maxPolygon, "the canvas draws at most maxPolygon vertices");

public:
    explicit polygonShape(nodeContext& c) : drawing(c), paint_(c, paintSource::parts::fillAndStroke) {
        for (std::size_t i = 0; i < N; ++i) {
            const std::string k = std::to_string(i + 1);
            xs_[i] = c.frameParam("x" + k);
            ys_[i] = c.frameParam("y" + k);
        }
    }

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        frame& f = begin();
        std::array<vertex, N> corners{};
        for (std::size_t i = 0; i < N; ++i) corners[i] = {.x = xs_[i].value(), .y = ys_[i].value()};
        f.drawing().polygon(corners, paint_.now(), f.transform());
    }

private:
    std::array<frameValue, N> xs_;
    std::array<frameValue, N> ys_;
    paintSource paint_;
};

class arcShape final : public drawing {
public:
    explicit arcShape(nodeContext& c)
        : drawing(c),
          x_(c.frameParam("x")),
          y_(c.frameParam("y")),
          w_(c.frameParam("width")),
          h_(c.frameParam("height")),
          start_(c.frameParam("start")),
          stop_(c.frameParam("stop")),
          mode_(static_cast<arcMode>(c.choice("mode"))),
          paint_(c, paintSource::parts::fillAndStroke) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        frame& f = begin();
        const box bounds = placed(placement::center, x_.value(), y_.value(), w_.value(), h_.value());
        f.drawing().arc(bounds, start_.value(), stop_.value(), mode_, paint_.now(), f.transform());
    }

private:
    frameValue x_;
    frameValue y_;
    frameValue w_;
    frameValue h_;
    frameValue start_;
    frameValue stop_;
    arcMode mode_ = arcMode::open;
    paintSource paint_;
};

class bezierShape final : public drawing {
public:
    explicit bezierShape(nodeContext& c)
        : drawing(c),
          coords_{c.frameParam("x1"),  c.frameParam("y1"),  c.frameParam("cx1"), c.frameParam("cy1"),
                  c.frameParam("cx2"), c.frameParam("cy2"), c.frameParam("x2"),  c.frameParam("y2")},
          paint_(c, paintSource::parts::strokeOnly) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        frame& f = begin();
        const auto at = [this](std::size_t i) noexcept {
            return vertex{.x = coords_[2 * i].value(), .y = coords_[(2 * i) + 1].value()};
        };
        f.drawing().bezier(at(0), at(1), at(2), at(3), paint_.now(), f.transform());
    }

private:
    std::array<frameValue, 8> coords_;  // from, control 1, control 2, to
    paintSource paint_;
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
class transform final : public drawing {
public:
    explicit transform(nodeContext& c)
        : drawing(c),
          tx_(c.frameParam("translate-x")),
          ty_(c.frameParam("translate-y")),
          angle_(c.frameParam("rotate")),
          sx_(c.frameParam("scale-x")),
          sy_(c.frameParam("scale-y")) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        begin().setTransform(composed(tx_.value(), ty_.value(), angle_.value(), sx_.value(), sy_.value()));
    }

private:
    frameValue tx_;
    frameValue ty_;
    frameValue angle_;
    frameValue sx_;
    frameValue sy_;
};

std::shared_ptr<primitive> makeRect(nodeContext& c) {
    const auto mode = rectModes.at(static_cast<std::size_t>(c.choice("mode")));
    return std::make_shared<boxed>(c, false, "width", "height", mode);
}

std::shared_ptr<primitive> makeEllipse(nodeContext& c) {
    const auto mode = ellipseModes.at(static_cast<std::size_t>(c.choice("mode")));
    return std::make_shared<boxed>(c, true, "width", "height", mode);
}

// A circle is placed by its center.
std::shared_ptr<primitive> makeCircle(nodeContext& c) {
    return std::make_shared<boxed>(c, true, "diameter", "diameter", placement::center);
}

// A color between two colors, per block, to the four channels (R, G, B,
// A) of a control pipe that image parameters follow (§7.7). It draws
// nothing, so it runs on the audio thread like any control generator.
class lerpColor final : public nodeBase {
public:
    explicit lerpColor(nodeContext& c)
        : nodeBase(c),
          a_(c.colorParam("color-a")),
          b_(c.colorParam("color-b")),
          amount_(c.param("amount")),
          out_(c.controlOut("out")) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        if (!out_.pipe) return;
        const double t = std::clamp(static_cast<double>(amount_.value()), 0.0, 1.0);
        const metadata::rgba a = a_.value().value_or(metadata::rgba{});
        const metadata::rgba b = b_.value().value_or(metadata::rgba{});
        for (int ch = 0; ch < std::min(out_.channels, metadata::colorChannels); ++ch) {
            const auto i = static_cast<std::size_t>(ch);
            emit(*out_.pipe, ch, static_cast<float>(a[i] + ((b[i] - a[i]) * t)));
        }
    }

private:
    frameColor a_;
    frameColor b_;
    signal amount_;
    controlOutput out_;
};

// An image file drawn at (x, y), scaled to width × height, or its own size
// where those are 0. The file is decoded when the plan is built, so the
// render thread only blits (§7.7.6).
class imagePlayer final : public drawing {
public:
    explicit imagePlayer(nodeContext& c)
        : drawing(c),
          picture_(c.loadImage("image")),
          x_(c.frameParam("x")),
          y_(c.frameParam("y")),
          w_(c.frameParam("width")),
          h_(c.frameParam("height")) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {
        frame& f = begin();
        const double w = w_.value();
        const double h = h_.value();
        const box dest{.x = x_.value(),
                       .y = y_.value(),
                       .width = w > 0.0 ? w : picture_.width(),
                       .height = h > 0.0 ? h : picture_.height()};
        f.drawing().blit(picture_.drawing(), dest, f.transform());
    }

private:
    frame picture_;
    frameValue x_;
    frameValue y_;
    frameValue w_;
    frameValue h_;
};

// The visual layer (v1 has one): the server shows its input frame. It
// draws nothing; the plan records the frame it shows.
class videoOut final : public nodeBase {
public:
    explicit videoOut(nodeContext& c) : nodeBase(c) {}

    void process(const blockContext& /*ctx*/) noexcept PROJ_NONBLOCKING override {}
};

}  // namespace

std::span<const factoryEntry> imageFactories() {
    static const auto factories = std::to_array<factoryEntry>({
        {.type = "background", .make = make<background>},
        {.type = "point", .make = make<pointShape>},
        {.type = "line", .make = make<lineShape>},
        {.type = "rect", .make = makeRect},
        {.type = "ellipse", .make = makeEllipse},
        {.type = "circle", .make = makeCircle},
        {.type = "triangle", .make = make<polygonShape<3>>},
        {.type = "quad", .make = make<polygonShape<4>>},
        {.type = "arc", .make = make<arcShape>},
        {.type = "bezier", .make = make<bezierShape>},
        {.type = "transform", .make = make<transform>},
        {.type = "lerpColor", .make = make<lerpColor>},
        {.type = "imagePlayer", .make = make<imagePlayer>},
        {.type = "videoOut", .make = make<videoOut>},
    });
    return factories;
}

}  // namespace primitives
