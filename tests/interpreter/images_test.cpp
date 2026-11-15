// Image generators in the language: video pipes, colors, and calls
// (primitives.md §7.7).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include <proj/interpreter/compiler.h>
#include <proj/metadata/graph.h>

#include "support/unwrap.h"

using Catch::Matchers::ContainsSubstring;
using namespace interpreter;

namespace {

compileResult piece(const std::string& body) {
    return compile("(defpiece main ()\n" + body + ")\n", "sketch.lisp");
}

metadata::graphSpec ok(const compileResult& r) {
    INFO(r.text());
    REQUIRE(r.ok());
    return support::unwrap(r.graph);
}

std::string error(const compileResult& r) {
    REQUIRE_FALSE(r.ok());
    return r.text();
}

const metadata::nodeSpec& find(const metadata::graphSpec& g, const std::string& type) {
    for (const auto& n : g.nodes) {
        if (n.type == type) return n;
    }
    FAIL("no node of type " + type);
    return g.nodes.front();
}

std::vector<double> colorOf(const metadata::nodeSpec& n, const std::string& param) {
    return std::get<std::vector<double>>(n.params.at(param));
}

}  // namespace

TEST_CASE("a sketch compiles to video pipes and image nodes") {
    const auto g = ok(piece(R"(
  (let ((frame (pipe :video))
        (drawn (pipe :video)))
    (background :color (rgb 0 0 0) :out frame)
    (rect :in frame :x 10 :y 10 :width 20 :height 5 :fill (rgb 1 0 0) :stroke :none :out drawn)
    (video-out :in drawn)))"));
    CHECK(colorOf(find(g, "background"), "color") == std::vector<double>{0.0, 0.0, 0.0, 1.0});
    const auto& box = find(g, "rect");
    CHECK(colorOf(box, "fill") == std::vector<double>{1.0, 0.0, 0.0, 1.0});
    CHECK(std::get<std::string>(box.params.at("stroke")) == "none");
    CHECK(find(g, "videoOut").inputs.at("in").front().kind == metadata::pipeKind::video);
}

TEST_CASE("hsb converts to RGB, with optional alpha") {
    const auto g = ok(piece(R"(
  (let ((a (pipe :video)) (b (pipe :video)))
    (background :color (hsb 0 1 1) :out a)
    (background :in a :color (hsb 0.5 1 1 0.5) :out b)
    (video-out :in b)))"));
    CHECK(colorOf(g.nodes[0], "color") == std::vector<double>{1.0, 0.0, 0.0, 1.0});  // red
    CHECK(colorOf(g.nodes[1], "color") == std::vector<double>{0.0, 1.0, 1.0, 0.5});  // cyan, half alpha
}

TEST_CASE("a fill follows a 4-channel pipe written by lerp-color") {
    const auto g = ok(piece(R"(
  (let ((c (pipe :control :channels 4)) (frame (pipe :video)))
    (lerp-color :color-a (rgb 1 0 0) :color-b (rgb 0 0 1) :amount 0.5 :out c)
    (circle :x 50 :y 50 :diameter 20 :fill c :out frame)
    (video-out :in frame)))"));
    CHECK(std::holds_alternative<metadata::pipeRef>(find(g, "circle").params.at("fill")));
}

TEST_CASE("bad colors are errors where they are written") {
    const auto with = [](const std::string& call) {
        return error(piece("(let ((c (pipe :control)) (v (pipe :video))) " + call + " (video-out :in v))"));
    };
    CHECK_THAT(with("(rect :fill (rgb 2 0 0) :out v)"),
               ContainsSubstring("rgb takes plain numbers from 0 to 1"));
    CHECK_THAT(with("(rect :fill 3 :out v)"),
               ContainsSubstring(":fill expects a color, such as (rgb 1 0.5 0) or :none"));
    CHECK_THAT(with("(background :color :none :out v)"),
               ContainsSubstring(":color takes a color, not :none"));
    CHECK_THAT(with("(rect :fill c :out v)"), ContainsSubstring(":fill must follow a pipe of 4 channels"));
    CHECK_THAT(with("(rect :fill (rgb 1 0) :out v)"),
               ContainsSubstring("expected (rgb r g b) or with alpha"));
    // A color written as a list takes the same plain numbers as rgb.
    CHECK_THAT(with("(rect :fill (list 0.5hz 0 0 1) :out v)"), ContainsSubstring(":fill expects a color"));
}
