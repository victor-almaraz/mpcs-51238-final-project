// Image generators (primitives.md §7.7): what each one draws.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include <proj/metadata/graph.h>
#include <proj/primitives/engine.h>
#include <proj/primitives/frame.h>

#include "support/graph.h"
#include "support/unwrap.h"

using namespace support;
using namespace primitives;

namespace {

constexpr std::uint32_t black = 0xff000000u;
constexpr std::uint32_t red = 0xffff0000u;
constexpr std::uint32_t blue = 0xff0000ffu;
constexpr std::uint32_t white = 0xffffffffu;
constexpr int size = 20;

using params = std::map<std::string, metadata::paramValue, std::less<>>;

// A chain of @p steps (type and parameters) drawing over an opaque black
// frame, the last one shown by the visual layer, rendered once on a
// 20 × 20 frame.
frame chain(const std::vector<std::pair<std::string, params>>& steps) {
    graph g;
    const int last = static_cast<int>(steps.size());
    for (int i = 0; i <= last; ++i) g.pipe(video(i));
    auto& base = g.node("base", "background");
    base.params["color"] = std::vector<double>{0.0, 0.0, 0.0, 1.0};
    base.outputs["out"] = {video(0)};
    for (int i = 0; i < last; ++i) {
        const auto& [type, p] = steps[static_cast<std::size_t>(i)];
        auto& n = g.node("step" + std::to_string(i), type);
        n.params = p;
        n.inputs["in"] = {video(i)};
        n.outputs["out"] = {video(i + 1)};
    }
    g.node("show", "videoOut").inputs["in"] = {video(last)};

    engine e({.audio = {.sampleRate = 48000.0, .blockSize = 64, .frameWidth = size, .frameHeight = size},
              .pipes = {.video = 8}});
    e.play(e.build(g.spec, 1), 0.0, quantization::none);
    e.process({}, 64, 0.0);
    e.collect();
    frame out(size, size);
    out.copyFrom(unwrap(e.renderFrame(0.0)).get());
    return out;
}

// One @p type node drawing over an opaque black frame.
frame drawn(const std::string& type, params p) { return chain({{type, std::move(p)}}); }

metadata::paramValue none() { return std::string("none"); }
metadata::paramValue redColor() { return std::vector<double>{1.0, 0.0, 0.0, 1.0}; }
metadata::paramValue blueColor() { return std::vector<double>{0.0, 0.0, 1.0, 1.0}; }

bool samePixels(const frame& a, const frame& b) {
    return std::ranges::equal(a.pixels(), b.pixels());
}

} // namespace

TEST_CASE("rect fills its corner box (primitives.md 7.7)") {
    const frame f = drawn("rect", {{"x", 5.0}, {"y", 5.0}, {"width", 10.0}, {"height", 10.0},
                                   {"fill", redColor()}, {"stroke", none()}});
    CHECK(f.at(5, 5) == red);  // pixels 5 to 14 are covered
    CHECK(f.at(14, 14) == red);
    CHECK(f.at(15, 15) == black);
    CHECK(f.at(4, 10) == black);
}

TEST_CASE("rect strokes its outline without a fill") {
    const frame f = drawn("rect", {{"x", 5.0}, {"y", 5.0}, {"width", 10.0}, {"height", 10.0},
                                   {"fill", none()}, {"stroke", blueColor()}, {"stroke-weight", 2.0}});
    CHECK(f.at(4, 10) == blue);  // the stroke covers x 4 to 6 around the edge at 5
    CHECK(f.at(5, 10) == blue);
    CHECK(f.at(10, 10) == black);  // no fill
}

TEST_CASE("rect paints Processing's default white fill and black stroke") {
    const frame f = drawn("rect", {{"x", 5.0}, {"y", 5.0}, {"width", 10.0}, {"height", 10.0}});
    CHECK(f.at(10, 10) == white);
}

TEST_CASE("rect modes place the same box") {
    const frame corner = drawn("rect", {{"x", 5.0}, {"y", 5.0}, {"width", 10.0}, {"height", 10.0},
                                        {"fill", redColor()}, {"stroke", none()}});
    const auto with = [](std::string mode, double x, double y, double w, double h) {
        return drawn("rect", {{"x", x}, {"y", y}, {"width", w}, {"height", h}, {"mode", std::move(mode)},
                              {"fill", redColor()}, {"stroke", none()}});
    };
    CHECK(samePixels(with("center", 10.0, 10.0, 10.0, 10.0), corner));
    CHECK(samePixels(with("radius", 10.0, 10.0, 5.0, 5.0), corner));
    CHECK(samePixels(with("corners", 5.0, 5.0, 15.0, 15.0), corner));
    CHECK(samePixels(with("corners", 15.0, 15.0, 5.0, 5.0), corner));  // either corner first
}

TEST_CASE("ellipse fills inside its bounds and not their corners") {
    const frame f = drawn("ellipse", {{"x", 10.0},
                                      {"y", 10.0},
                                      {"width", 10.0},
                                      {"height", 10.0},
                                      {"fill", redColor()},
                                      {"stroke", none()}});
    CHECK(f.at(10, 10) == red);
    CHECK(f.at(12, 9) == red);
    CHECK(f.at(5, 5) == black);  // the bounding box's corner lies outside
    CHECK(f.at(16, 10) == black);
}

TEST_CASE("ellipse modes place the same ellipse and circle matches a centered one") {
    const auto with = [](std::string mode, double x, double y, double w, double h) {
        return drawn("ellipse", {{"x", x},
                                 {"y", y},
                                 {"width", w},
                                 {"height", h},
                                 {"mode", std::move(mode)},
                                 {"fill", redColor()},
                                 {"stroke", none()}});
    };
    const frame center = with("center", 10.0, 10.0, 10.0, 10.0);
    CHECK(samePixels(with("radius", 10.0, 10.0, 5.0, 5.0), center));
    CHECK(samePixels(with("corner", 5.0, 5.0, 10.0, 10.0), center));
    CHECK(samePixels(with("corners", 15.0, 15.0, 5.0, 5.0), center));
    const frame circle = drawn(
        "circle", {{"x", 10.0}, {"y", 10.0}, {"diameter", 10.0}, {"fill", redColor()}, {"stroke", none()}});
    CHECK(samePixels(circle, center));
}

TEST_CASE("line strokes between its ends and its cap sets how far it reaches") {
    const auto with = [](std::string cap) {
        return drawn("line", {{"x1", 4.0},
                              {"y1", 10.0},
                              {"x2", 16.0},
                              {"y2", 10.0},
                              {"stroke", redColor()},
                              {"stroke-weight", 2.0},
                              {"stroke-cap", std::move(cap)}});
    };
    const frame square = with("square");  // flat at the end points
    CHECK(square.at(10, 9) == red);
    CHECK(square.at(10, 10) == red);
    CHECK(square.at(10, 12) == black);
    CHECK(square.at(3, 10) == black);
    const frame project = with("project");  // half the weight past them
    CHECK(project.at(3, 10) == red);
    CHECK(project.at(2, 10) == black);
}

TEST_CASE("point draws a dot of the stroke weight, round or square") {
    const auto with = [](std::string cap) {
        return drawn("point", {{"x", 10.0},
                               {"y", 10.0},
                               {"stroke", redColor()},
                               {"stroke-weight", 6.0},
                               {"stroke-cap", std::move(cap)}});
    };
    const frame round = with("round");
    CHECK(round.at(10, 10) == red);
    CHECK(round.at(7, 6) == black);  // wholly outside the round dot of radius 3
    const frame square = with("square");
    CHECK(square.at(7, 7) == red);  // the square dot covers 7 to 12
    CHECK(square.at(13, 13) == black);
}

TEST_CASE("triangle fills between its three points") {
    const frame f = drawn("triangle", {{"x1", 2.0},
                                       {"y1", 18.0},
                                       {"x2", 18.0},
                                       {"y2", 18.0},
                                       {"x3", 10.0},
                                       {"y3", 2.0},
                                       {"fill", redColor()},
                                       {"stroke", none()}});
    CHECK(f.at(10, 14) == red);  // inside, near the base
    CHECK(f.at(3, 4) == black);  // outside, beside the apex
    CHECK(f.at(16, 4) == black);
}

TEST_CASE("quad through a square's corners covers the same pixels as rect") {
    const frame q = drawn("quad", {{"x1", 5.0},
                                   {"y1", 5.0},
                                   {"x2", 15.0},
                                   {"y2", 5.0},
                                   {"x3", 15.0},
                                   {"y3", 15.0},
                                   {"x4", 5.0},
                                   {"y4", 15.0},
                                   {"fill", redColor()},
                                   {"stroke", none()}});
    const frame r = drawn("rect", {{"x", 5.0},
                                   {"y", 5.0},
                                   {"width", 10.0},
                                   {"height", 10.0},
                                   {"fill", redColor()},
                                   {"stroke", none()}});
    CHECK(samePixels(q, r));
}

TEST_CASE("arc draws clockwise from start to stop in its mode") {
    const auto with = [](std::string mode) {
        return drawn("arc", {{"x", 10.0},
                             {"y", 10.0},
                             {"width", 16.0},
                             {"height", 16.0},
                             {"start", 0.0},
                             {"stop", std::numbers::pi / 2.0},
                             {"mode", std::move(mode)},
                             {"fill", redColor()},
                             {"stroke", none()}});
    };
    const frame pie = with("pie");  // from 3 o'clock clockwise to 6 o'clock: the lower right quarter
    CHECK(pie.at(13, 13) == red);
    CHECK(pie.at(11, 11) == red);
    CHECK(pie.at(6, 6) == black);
    CHECK(pie.at(13, 6) == black);
    const frame chord = with("chord");  // cut by the line between the arc's ends
    CHECK(chord.at(14, 14) == red);
    CHECK(chord.at(11, 11) == black);
    CHECK(samePixels(with("open"), chord));  // open fills like chord; only its stroke differs
}

TEST_CASE("bezier strokes its curve") {
    const frame f = drawn("bezier", {{"x1", 4.0},
                                     {"y1", 10.0},
                                     {"cx1", 8.0},
                                     {"cy1", 10.0},
                                     {"cx2", 12.0},
                                     {"cy2", 10.0},
                                     {"x2", 16.0},
                                     {"y2", 10.0},
                                     {"stroke", redColor()},
                                     {"stroke-weight", 2.0}});
    CHECK(f.at(10, 9) == red);  // a straight curve along y = 10
    CHECK(f.at(10, 10) == red);
    CHECK(f.at(10, 13) == black);
}

TEST_CASE("transform moves, scales, and rotates the drawing downstream (primitives.md 7.7.2)") {
    const params square{{"x", 5.0},       {"y", 5.0},           {"width", 10.0},
                        {"height", 10.0}, {"fill", redColor()}, {"stroke", none()}};
    const frame plain = drawn("rect", square);
    CHECK(samePixels(chain({{"transform", {{"translate-x", 5.0}, {"translate-y", 5.0}}},
                            {"rect",
                             {{"x", 0.0},
                              {"y", 0.0},
                              {"width", 10.0},
                              {"height", 10.0},
                              {"fill", redColor()},
                              {"stroke", none()}}}}),
                     plain));
    CHECK(samePixels(chain({{"transform", {{"scale-x", 2.0}, {"scale-y", 2.0}}},
                            {"rect",
                             {{"x", 2.5},
                              {"y", 2.5},
                              {"width", 5.0},
                              {"height", 5.0},
                              {"fill", redColor()},
                              {"stroke", none()}}}}),
                     plain));
    // Rotated half a turn about the origin, then moved: 5..15 becomes -15..-5,
    // then 5..15. sin(pi) is not exactly 0, so edges may differ in their
    // last bit of coverage: compare inside and outside, not every pixel.
    const frame turned =
        chain({{"transform", {{"rotate", std::numbers::pi}, {"translate-x", 20.0}, {"translate-y", 20.0}}},
               {"rect", square}});
    CHECK(turned.at(6, 6) == red);
    CHECK(turned.at(13, 13) == red);
    CHECK(turned.at(3, 10) == black);
    CHECK(turned.at(16, 10) == black);
}

TEST_CASE("a later transform replaces an earlier one") {
    const params square{{"x", 5.0},       {"y", 5.0},           {"width", 10.0},
                        {"height", 10.0}, {"fill", redColor()}, {"stroke", none()}};
    CHECK(samePixels(chain({{"transform", {{"translate-x", 3.0}}}, {"transform", {}}, {"rect", square}}),
                     drawn("rect", square)));
}
