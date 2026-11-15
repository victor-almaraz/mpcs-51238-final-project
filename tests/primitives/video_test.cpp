// The video domain: render-thread plans, the visual layer, and background
// (primitives.md §3, §5.5, §7.7).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <proj/metadata/graph.h>
#include <proj/metadata/primitiveSpec.h>
#include <proj/primitives/engine.h>
#include <proj/primitives/frame.h>
#include <proj/primitives/plan.h>
#include <proj/primitives/primitive.h>

#include "support/graph.h"
#include "support/unwrap.h"

using namespace support;
using namespace primitives;

namespace {

constexpr std::uint32_t red = 0xffff0000u;
constexpr std::uint32_t blue = 0xff0000ffu;

engineOptions withVideo() {
    return {.audio = {.sampleRate = 48000.0, .blockSize = 64, .frameWidth = 8, .frameHeight = 6},
            .pipes = {.video = 2}};
}

// One audio block, then the control thread's collect: the swap activates
// and its plan goes to the render thread.
void activate(engine& e) {
    e.process({}, e.info().blockSize, 0.0);
    e.collect();
}

}  // namespace

TEST_CASE("video nodes run on the render thread, not the audio thread (primitives.md 3)") {
    engine e(withVideo());
    const auto p = e.build(shown({1.0, 0.0, 0.0, 1.0}), 1);
    CHECK(p->nodes.empty());
    CHECK(p->videoNodes.size() == 1);  // background; videoOut only names the frame shown
    CHECK(p->display != nullptr);
}

TEST_CASE("the visual layer shows a plan once it is active") {
    engine e(withVideo());
    e.play(e.build(shown({1.0, 0.0, 0.0, 1.0}), 1), 0.0, quantization::none);
    CHECK_FALSE(e.renderFrame(0.0));  // not yet active
    activate(e);
    const frame& f = unwrap(e.renderFrame(0.0)).get();
    CHECK(f.at(0, 0) == red);
    CHECK(f.at(7, 5) == red);
}

TEST_CASE("a newer plan replaces the shown one and a stop clears it") {
    engine e(withVideo());
    e.play(e.build(shown({1.0, 0.0, 0.0, 1.0}), 1), 0.0, quantization::none);
    activate(e);
    REQUIRE(e.renderFrame(0.0));
    e.play(e.build(shown({0.0, 0.0, 1.0, 1.0}), 2), 0.0, quantization::none);
    activate(e);
    CHECK(unwrap(e.renderFrame(1.0 / 60.0)).get().at(3, 3) == blue);
    e.stop(0.0);
    activate(e);
    CHECK_FALSE(e.renderFrame(2.0 / 60.0));
    e.collect();  // the plans the render thread let go are freed here
}

TEST_CASE("background composites over its input frame") {
    engine e(withVideo());
    graph g;
    g.pipe(video(0));
    g.pipe(video(1));
    auto& base = g.node("base", "background");
    base.params["color"] = std::vector<double>{1.0, 0.0, 0.0, 1.0};
    base.outputs["out"] = {video(0)};
    auto& veil = g.node("veil", "background");
    veil.params["color"] = std::vector<double>{0.0, 0.0, 1.0, 0.5};
    veil.inputs["in"] = {video(0)};
    veil.outputs["out"] = {video(1)};
    g.node("show", "videoOut").inputs["in"] = {video(1)};
    e.play(e.build(g.spec, 1), 0.0, quantization::none);
    activate(e);
    // The same compositing as frame::fill, whose values frame_test checks.
    frame want(1, 1);
    want.fill({1.0, 0.0, 0.0, 1.0});
    want.fill({0.0, 0.0, 1.0, 0.5});
    CHECK(unwrap(e.renderFrame(0.0)).get().at(1, 1) == want.at(0, 0));
}

TEST_CASE("an engine without video pipes builds no video plan and shows nothing") {
    engine e;
    CHECK_THROWS_AS(e.build(shown({1.0, 0.0, 0.0, 1.0}), 1), buildError);
    CHECK_FALSE(e.renderFrame(0.0));
}

TEST_CASE("an image parameter follows a control pipe or an audio pipe's latest sample (primitives.md 8.1)") {
    engine e(withVideo());
    graph g;
    g.pipe(control(1));
    g.pipe(audio(0));
    g.pipe(video(0));
    // A sine at 0 Hz from phase 0.25 holds 1: the audio pipe's latest sample.
    auto& one = g.node("one", "sineOsc");
    one.params = {{"freq", 0.0}, {"phase", 0.25}};
    one.outputs["out"] = {audio(0)};
    auto& box = g.node("box", "rect");
    box.params = {{"x", control(1)},
                  {"y", audio(0)},
                  {"width", 2.0},
                  {"height", 2.0},
                  {"fill", std::vector<double>{1.0, 0.0, 0.0, 1.0}},
                  {"stroke", std::string("none")}};
    box.outputs["out"] = {video(0)};
    g.node("show", "videoOut").inputs["in"] = {video(0)};
    e.play(e.build(g.spec, 1), 0.0, quantization::none);
    REQUIRE(e.setControl(1, 5.0f));
    activate(e);  // the block applies the control value and writes the sine
    const frame& f = unwrap(e.renderFrame(0.0)).get();
    CHECK(f.at(5, 1) == red);  // x from the control pipe, y from the audio pipe
    CHECK(f.at(6, 2) == red);
    CHECK(f.at(4, 1) == 0u);
    CHECK(f.at(5, 0) == 0u);
    REQUIRE(e.setControl(1, 2.0f));
    e.process({}, e.info().blockSize, 0.0);
    CHECK(unwrap(e.renderFrame(1.0 / 60.0)).get().at(2, 1) == red);  // follows the pipe frame to frame
}

TEST_CASE("line ends and transforms follow pipes too") {
    engine e(withVideo());
    graph g;
    g.pipe(control(1));
    g.pipe(video(0));
    g.pipe(video(1));
    auto& move = g.node("move", "transform");
    move.params = {{"translate-y", control(1)}};
    move.outputs["out"] = {video(0)};
    auto& stroke = g.node("stroke", "line");
    stroke.params = {{"x1", 0.0},
                     {"y1", 0.5},
                     {"x2", control(1)},
                     {"y2", 0.5},
                     {"stroke-cap", std::string("square")},
                     {"stroke", std::vector<double>{1.0, 0.0, 0.0, 1.0}}};
    stroke.inputs["in"] = {video(0)};
    stroke.outputs["out"] = {video(1)};
    g.node("show", "videoOut").inputs["in"] = {video(1)};
    e.play(e.build(g.spec, 1), 0.0, quantization::none);
    REQUIRE(e.setControl(1, 3.0f));
    activate(e);
    const frame& f = unwrap(e.renderFrame(0.0)).get();
    CHECK(f.at(2, 3) == red);  // x from 0 to 3, moved down by 3
    CHECK(f.at(4, 3) == 0u);
    CHECK(f.at(1, 0) == 0u);
}

TEST_CASE("a fill follows a color pipe written by lerpColor") {
    engine e(withVideo());
    graph g;
    g.pipe(control(2), metadata::colorChannels);
    g.pipe(video(0));
    auto& mix = g.node("mix", "lerpColor");
    mix.params = {{"color-a", std::vector<double>{1.0, 0.0, 0.0, 1.0}},
                  {"color-b", std::vector<double>{0.0, 0.0, 1.0, 1.0}},
                  {"amount", 1.0}};
    mix.outputs["out"] = {control(2)};
    auto& box = g.node("box", "rect");
    box.params = {{"x", 0.0},      {"y", 0.0},           {"width", 8.0},
                  {"height", 6.0}, {"fill", control(2)}, {"stroke", std::string("none")}};
    box.outputs["out"] = {video(0)};
    g.node("show", "videoOut").inputs["in"] = {video(0)};
    e.play(e.build(g.spec, 1), 0.0, quantization::none);
    activate(e);  // the audio block writes the color; the render reads it
    CHECK(unwrap(e.renderFrame(0.0)).get().at(4, 3) == blue);
}
