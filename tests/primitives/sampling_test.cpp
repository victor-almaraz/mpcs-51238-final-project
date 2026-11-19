// bufferWrite and bufferRead: recording, overdub, freeze, once and loop,
// reset, read positions, interpolation, the seam fade, and swaps
// (samples.md §5.4, §5.5, §6).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/plan.h>

#include "support/graph.h"

using Catch::Approx;
using support::audio;
using support::control;

namespace {

// 1 kHz, so a frame is a millisecond: the 5 ms record ramp is 5 samples
// and the 10 ms seam fade 10. Tests that listen at the outputs keep their
// signals below 1, which the output limiter passes unchanged.
constexpr double rate = 1000.0;
constexpr int block = 10;

// A buffer declaration of @p frames frames at 1 kHz.
metadata::bufferDecl ring(double frames, int channels = 1, std::string id = "ring") {
    return {.buffer = {.index = 0},
            .id = std::move(id),
            .channels = channels,
            .seconds = frames / rate,
            .file = std::nullopt,
            .src = std::nullopt};
}

// An engine at 1 kHz, its device input fed by @p input (sample index ->
// value), its two outputs recorded.
class rig {
public:
    explicit rig(std::function<float(std::int64_t)> input = [](std::int64_t) { return 0.0f; })
        : engine({.audio = {.sampleRate = rate, .blockSize = block}, .pipes = {}}),
          input_(std::move(input)) {}

    std::shared_ptr<primitives::plan> play(const metadata::graphSpec& g, std::int32_t id = 1,
                                           double fade = 0.0) {
        auto p = engine.build(g, id);
        engine.play(p, fade, primitives::quantization::none);
        return p;
    }

    // Renders @p n samples, in blocks of up to `block`.
    void run(int n) {
        for (int done = 0; done < n;) {
            const int m = std::min(block, n - done);
            const auto count = static_cast<std::size_t>(m);
            std::vector<float> in(count);
            for (std::size_t i = 0; i < count; ++i) in[i] = input_(clock_ + static_cast<std::int64_t>(i));
            std::vector<float> out0(count);
            std::vector<float> out1(count);
            const std::vector<std::span<const float>> ins{in};
            const std::vector<std::span<float>> outs{out0, out1};
            engine.process({.inputs = ins, .outputs = outs}, m, static_cast<double>(clock_) / rate);
            out[0].insert(out[0].end(), out0.begin(), out0.end());
            out[1].insert(out[1].end(), out1.begin(), out1.end());
            clock_ += m;
            done += m;
        }
    }

    primitives::engine engine;
    std::array<std::vector<float>, 2> out;

private:
    std::function<float(std::int64_t)> input_;
    std::int64_t clock_ = 0;
};

// The device input recorded into the ring: audioIn -> bufferWrite.
support::graph recorder(metadata::bufferDecl b, std::string id = "write") {
    support::graph g;
    g.pipe(audio(0)).pipe(control(1));
    g.spec.buffers = {std::move(b)};
    g.node("in", "audioIn").outputs["out"] = {audio(0)};
    auto& w = g.node(std::move(id), "bufferWrite");
    w.params["buffer"] = metadata::bufferRef{.index = 0};
    w.inputs["in"] = {audio(0)};
    w.outputs["position"] = {control(1)};
    return g;
}

// A reader of the ring at @p position (seconds, or a pipe) to output 0.
metadata::nodeSpec& addReader(support::graph& g, metadata::paramValue position, std::string mode = "cubic") {
    g.pipe(audio(9));
    auto& r = g.node("read", "bufferRead");
    r.params["buffer"] = metadata::bufferRef{.index = 0};
    r.params["position"] = std::move(position);
    r.params["interpolation"] = std::move(mode);
    r.outputs["out"] = {audio(9)};
    g.node("out", "audioOut").inputs["in"] = {audio(9)};
    return r;
}

std::vector<float> contents(const primitives::sampleBuffer& b, int channel = 0) {
    const auto c = b.channel(channel);
    return {c.begin(), c.end()};
}

}  // namespace

TEST_CASE("bufferWrite records its input, ramping in over 5 ms (samples.md 6.2)") {
    rig r([](std::int64_t t) { return static_cast<float>(t + 1); });
    const auto p = r.play(recorder(ring(20)).spec);
    r.run(10);
    const auto b = contents(*p->buffers[0]);
    // The ramp reaches 0.2, 0.4, ... 1.0 over the first five samples.
    for (std::size_t i = 0; i < 5; ++i) {
        const auto k = static_cast<double>(i + 1);
        CHECK(b[i] == Approx(k * 0.2 * k).epsilon(1e-5));
    }
    for (std::size_t i = 5; i < 10; ++i) CHECK(b[i] == static_cast<float>(i + 1));
    CHECK(b[10] == 0.0f);  // not yet written
}

TEST_CASE("bufferWrite with keep overdubs, fading older layers by keep each pass") {
    rig r([](std::int64_t) { return 1.0f; });
    auto g = recorder(ring(20));
    g.spec.nodes[1].params["keep"] = 0.5;
    const auto p = r.play(g.spec);
    const auto at = [&] { return p->buffers[0]->channel(0)[10]; };
    r.run(20);
    CHECK(at() == Approx(1.0));
    r.run(20);
    CHECK(at() == Approx(1.5));
    r.run(20);
    CHECK(at() == Approx(1.75));
}

TEST_CASE("bufferWrite at record 0 holds its contents and its head: freeze") {
    rig r([](std::int64_t t) { return static_cast<float>(t); });
    auto g = recorder(ring(20));
    // Given in both plans, so the writer keeps its shape and is adopted.
    g.spec.nodes[1].params["record"] = 1.0;
    const auto p = r.play(g.spec);
    r.run(30);  // a pass and a half
    g.spec.nodes[1].params["record"] = 0.0;
    r.play(g.spec, 2);  // the same shape: the writer adopts its head and ramp
    r.run(10);
    const auto frozen = contents(*p->buffers[0]);
    r.run(100);
    CHECK(contents(*p->buffers[0]) == frozen);
    // The ramp down blended four more samples, at 0.8, 0.6, 0.4, and 0.2,
    // into frames 10 to 13; frame 14 keeps the first pass.
    CHECK(frozen[10] == Approx((10.0 * 0.2) + (30.0 * 0.8)).epsilon(1e-5));
    CHECK(frozen[13] == Approx((13.0 * 0.8) + (33.0 * 0.2)).epsilon(1e-5));
    CHECK(frozen[14] == 14.0f);
}

TEST_CASE("bufferWrite in once mode stops at the end until a reset") {
    rig r([](std::int64_t t) { return static_cast<float>(t); });
    auto g = recorder(ring(20));
    g.spec.nodes[1].params["mode"] = std::string("once");
    g.pipe(support::events(2));
    auto& metro = g.node("metro", "metro");  // a beat is 500 samples at 120 BPM
    metro.outputs["out"] = {support::events(2)};
    g.spec.nodes[1].inputs["reset"] = {support::events(2)};
    // metro must run before the writer that reads its events.
    std::swap(g.spec.nodes[1], g.spec.nodes[2]);
    const auto p = r.play(g.spec);
    r.run(400);
    CHECK(p->buffers[0]->channel(0)[19] == 19.0f);  // the first pass, then nothing
    r.run(140);
    // The beat at sample 500 sent the head back to the start.
    CHECK(p->buffers[0]->channel(0)[0] == 500.0f);
    CHECK(p->buffers[0]->channel(0)[19] == 519.0f);
}

TEST_CASE("bufferWrite maps its input's channels to the buffer's") {
    rig r([](std::int64_t) { return 0.5f; });
    const auto p = r.play(recorder(ring(20, 2)).spec);
    r.run(10);
    CHECK(p->buffers[0]->channel(0)[9] == 0.5f);
    CHECK(p->buffers[0]->channel(1)[9] == 0.5f);  // a mono input fills both
}

TEST_CASE("bufferRead reads at a position in seconds, wrapped to the buffer, interpolated") {
    // A ring whose frame k holds k / 100, recorded from the input.
    for (const auto& [mode, at525] :
         {std::pair{"cubic", 5.25f}, std::pair{"linear", 5.25f}, std::pair{"none", 5.0f}}) {
        INFO(mode);
        for (const auto& [position, expected] :
             {std::pair{0.00525, at525}, std::pair{-0.001, 19.0f}, std::pair{0.021, 1.0f}}) {
            INFO(position);
            rig r([](std::int64_t t) { return static_cast<float>(t % 20) / 100.0f; });
            auto g = recorder(ring(20));
            g.spec.nodes[1].params["keep"] = 0.0;
            addReader(g, position, mode);
            r.play(g.spec);
            r.run(40);  // two passes: every frame holds its index
            CHECK(r.out[0].back() == Approx(expected / 100.0).margin(1e-6));
        }
    }
}

TEST_CASE("a reader at a writer's position minus a delay plays the input that much later (samples.md 6.3)") {
    rig r([](std::int64_t t) { return static_cast<float>(t % 1000) / 2000.0f; });
    // A long ring, so the delay never meets a wrap.
    support::graph g = recorder(ring(1000));
    g.pipe(control(3));
    auto& behind = g.node("behind", "subtract");
    behind.inputs["in"] = {control(1)};
    behind.params["b"] = 0.007;  // 7 frames
    behind.outputs["out"] = {control(3)};
    addReader(g, control(3));
    r.play(g.spec);
    r.run(200);
    for (std::size_t t = 50; t < 200; ++t) {
        INFO(t);
        CHECK(r.out[0][t] == Approx(static_cast<double>(t - 7) / 2000.0).margin(1e-5));
    }
}

TEST_CASE("bufferRead follows a writer's head across the wrap without a sweep") {
    // A 30-frame ring: each block moves the heads a third of the way round.
    rig r([](std::int64_t t) { return static_cast<float>((t % 30) + 1) / 100.0f; });
    support::graph g = recorder(ring(30));
    g.pipe(control(3));
    auto& behind = g.node("behind", "subtract");
    behind.inputs["in"] = {control(1)};
    behind.params["b"] = 0.004;
    behind.outputs["out"] = {control(3)};
    addReader(g, control(3), "none");
    r.play(g.spec);
    r.run(200);
    for (std::size_t t = 60; t < 200; ++t) {
        INFO(t);
        CHECK(r.out[0][t] == static_cast<float>(((t - 4) % 30) + 1) / 100.0f);
    }
}

TEST_CASE("bufferRead crossfades over 10 ms when its position jumps (samples.md 6.4)") {
    // A 40-frame ring whose frame k holds k / 100, read at an audio position
    // that steps a frame a sample and jumps 20 frames ahead at sample 104.
    rig r([](std::int64_t t) {
        const std::int64_t frame = t < 104 ? t % 40 : ((t % 40) + 20) % 40;
        return static_cast<float>(frame) / 1000.0f;
    });
    support::graph g = recorder(ring(40));
    g.spec.nodes[1].params["record"] = 0.0;  // no recording: the frames are set below
    addReader(g, audio(0), "linear");
    const auto p = r.play(g.spec);
    for (std::size_t k = 0; k < 40; ++k) p->buffers[0]->channel(0)[k] = static_cast<float>(k) / 100.0f;
    r.run(130);
    const auto& y = r.out[0];
    CHECK(y[103] == Approx(0.23).margin(1e-6));
    // At the jump, from 23 to 4, the old head carries on from 24 and fades
    // out over ten samples while the new one fades in.
    for (std::size_t i = 0; i < 10; ++i) {
        const auto k = static_cast<double>(i);
        const double fade = (10.0 - k) / 10.0;
        CHECK(y[104 + i] == Approx((((4.0 + k) * (1.0 - fade)) + ((24.0 + k) * fade)) / 100.0).margin(1e-6));
    }
    CHECK(y[114] == Approx(0.14).margin(1e-6));
}

TEST_CASE("a swap keeps a writer's head and its buffer's contents (samples.md 5.4)") {
    rig r([](std::int64_t t) { return static_cast<float>(t); });
    const auto g = recorder(ring(20)).spec;
    const auto first = r.play(g);
    r.run(30);
    const auto second = r.play(g, 2);
    CHECK(second->buffers[0] == first->buffers[0]);
    r.run(10);
    // Recording carried on at frame 10 with no restart.
    for (std::size_t k = 10; k < 20; ++k)
        CHECK(second->buffers[0]->channel(0)[k] == static_cast<float>(20 + k));
}

TEST_CASE("a retiring writer does not write during the fade (samples.md 5.5)") {
    float level = 1.0f;
    rig r([&](std::int64_t) { return level; });
    const auto first = r.play(recorder(ring(20)).spec);
    r.run(27);  // a pass, and seven frames into the next
    // A writer under a new ID replaces it with a 10 ms crossfade; its head
    // starts at frame 0. The old head would have written frames 7 to 16.
    level = 0.25f;
    const auto second = r.play(recorder(ring(20), "replacement").spec, 2, 0.010);
    REQUIRE(second->buffers[0] == first->buffers[0]);
    r.run(10);
    const auto b = contents(*second->buffers[0]);
    for (std::size_t k = 10; k < 17; ++k) CHECK(b[k] == 1.0f);
}
