// bufferWrite and bufferRead: recording, overdub, freeze, once and loop,
// reset, read positions, interpolation, the seam fade, and swaps
// (samples.md §5.4, §5.5, §6).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/bessel.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/plan.h>

#include "support/bits.h"
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

constexpr double fileRate = 2.0 * rate;  // the files' own rate

// Silent files, at the rate asked for or at their own: kept.wav, 40
// frames, and twice.wav, 2000, at fileRate, twice the engine's; half.wav,
// 1000 frames at half the engine's.
std::shared_ptr<const primitives::decodedAudio> keptFiles(std::string_view path, double asked,
                                                          std::string& error) {
    std::size_t frames = 0;
    if (path == "kept.wav") frames = 40;
    if (path == "half.wav") frames = 1000;
    if (path == "twice.wav") frames = 2000;
    if (frames == 0) {
        error = "no such file";
        return nullptr;
    }
    const double own = path == "half.wav" ? rate / 2.0 : fileRate;
    return std::make_shared<primitives::decodedAudio>(primitives::decodedAudio{
        .channels = {std::vector<float>(frames)}, .sampleRate = asked > 0.0 ? asked : own});
}

// @p g with its buffer loaded from @p file at the file's own rate.
support::graph keptAt(support::graph g, std::string file) {
    auto& b = g.spec.buffers.at(0);
    b.seconds = std::nullopt;
    b.file = std::move(file);
    b.resample = false;
    return g;
}

float silence(std::int64_t) { return 0.0f; }
float silentFrame(std::size_t) { return 0.0f; }

// @p g with its buffer filled, when made, from fill.wav: a buffer's
// contents come from its file or a writer, and its levels (6.6) from them.
support::graph filled(support::graph g) {
    g.spec.buffers.at(0).file = "fill.wav";
    return g;
}

// keptFiles, and fill.wav, 20000 frames whose frame k is @p fill(k), at
// fileRate or the rate asked for.
primitives::sampleLoader loaderOf(std::function<float(std::size_t)> fill) {
    return [fill = std::move(fill)](std::string_view path, double asked, std::string& error) {
        if (path != "fill.wav") return keptFiles(path, asked, error);
        std::vector<float> x(20000);
        for (std::size_t k = 0; k < x.size(); ++k) x[k] = fill(k);
        return std::make_shared<const primitives::decodedAudio>(primitives::decodedAudio{
            .channels = {std::move(x)}, .sampleRate = asked > 0.0 ? asked : fileRate});
    };
}

// An engine at 1 kHz that loads keptFiles and fill.wav, from @p fill, its
// device input fed by @p input (sample index -> value), its two outputs
// recorded.
class rig {
public:
    explicit rig(std::function<float(std::int64_t)> input = silence,
                 std::function<float(std::size_t)> fill = silentFrame)
        : r_({.audio = {.sampleRate = rate, .blockSize = block},
              .pipes = {},
              .loadImage = nullptr,
              .loadSample = loaderOf(std::move(fill)),
              .bufferMemory = primitives::bufferPool::defaultLimit},
             2),
          input_(std::move(input)) {}

    std::shared_ptr<primitives::plan> play(const metadata::graphSpec& g, std::int32_t id = 1,
                                           double fade = 0.0) {
        auto p = r_.engine.build(g, id);
        r_.engine.play(p, fade, primitives::quantization::none);
        return p;
    }

    // Renders @p n samples, the device input from the input function.
    void run(int n) {
        std::vector<float> in(static_cast<std::size_t>(n));
        for (std::size_t i = 0; i < in.size(); ++i) in[i] = input_(clock_ + static_cast<std::int64_t>(i));
        r_.run(n, {in});
        clock_ += n;
    }

    const std::vector<float>& out(int channel) const { return r_.out(channel); }

private:
    support::renderer r_;
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

TEST_CASE("bufferWrite records its input ramping in over 5 ms (samples.md 6.2)") {
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

TEST_CASE("bufferWrite with keep overdubs fading older layers by keep each pass") {
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

TEST_CASE("bufferRead reads at a position in seconds wrapped to the buffer interpolated") {
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
            CHECK(r.out(0).back() == Approx(expected / 100.0).margin(1e-6));
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
        CHECK(r.out(0)[t] == Approx(static_cast<double>(t - 7) / 2000.0).margin(1e-5));
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
        CHECK(r.out(0)[t] == static_cast<float>(((t - 4) % 30) + 1) / 100.0f);
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
    const auto& y = r.out(0);
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

namespace {

// A samplePlayer of a 40-frame ring, triggered by a metro every @p every
// beats at 937.5 BPM (a beat is 64 samples at 1 kHz, a sample exactly
// 1/64 beat, so beats land on their samples), through
// triggerNotes at @p pitch when given, to output 0. Its :done starts a
// second player, from frame 30, to output 1.
support::graph player(std::map<std::string, metadata::paramValue, std::less<>> params, double every,
                      std::optional<double> pitch = std::nullopt) {
    support::graph g;
    g.spec.tempo = 937.5;
    g.spec.buffers = {ring(40)};
    g.pipe(support::events(2))
        .pipe(support::events(3))
        .pipe(support::events(4))
        .pipe(audio(8))
        .pipe(audio(9));
    auto& clock = g.node("clock", "metro");
    clock.params["every"] = every;
    clock.outputs["out"] = {support::events(2)};
    if (pitch) {
        auto& notes = g.node("notes", "triggerNotes");
        notes.params["pitch"] = *pitch;
        notes.params["length"] = 0.005;
        notes.inputs["in"] = {support::events(2)};
        notes.outputs["out"] = {support::events(3)};
    }
    auto& p = g.node("play", "samplePlayer");
    p.params["buffer"] = metadata::bufferRef{.index = 0};
    for (auto& [name, value] : params) p.params[name] = value;
    p.inputs["trigger"] = {support::events(pitch ? 3 : 2)};
    p.outputs["out"] = {audio(9)};
    p.outputs["done"] = {support::events(4)};
    auto& after = g.node("after", "samplePlayer");
    after.params["buffer"] = metadata::bufferRef{.index = 0};
    after.params["start"] = 0.030;
    after.inputs["trigger"] = {support::events(4)};
    after.outputs["out"] = {audio(8)};
    g.node("out", "audioOut").inputs["in"] = {audio(9)};
    auto& second = g.node("out2", "audioOut");
    second.params["channel"] = 1.0;
    second.inputs["in"] = {audio(8)};
    return g;
}

// Plays @p g for @p n samples with frame k of the ring holding k / 100.
std::array<std::vector<float>, 2> listen(const support::graph& g, int n) {
    rig r;
    const auto p = r.play(g.spec);
    for (std::size_t k = 0; k < 40; ++k) p->buffers[0]->channel(0)[k] = static_cast<float>(k) / 100.0f;
    r.run(n);
    return {r.out(0), r.out(1)};
}

// Frame indices as the ring holds them.
float frame(double k) { return static_cast<float>(k / 100.0); }

// A sample index as a number.
double asDouble(std::size_t i) { return static_cast<double>(i); }

}  // namespace

TEST_CASE("samplePlayer plays from its start at a trigger and signals the end (samples.md 7.1)") {
    const auto y = listen(player({{"start", 0.005}}, 4.0), 100);
    for (std::size_t i = 0; i < 35; ++i) CHECK(y[0][i] == Approx(frame(5.0 + asDouble(i))).margin(1e-6));
    // Past the last frame, silence; :done started the second player there.
    for (std::size_t i = 35; i < 100; ++i) CHECK(y[0][i] == 0.0f);
    CHECK(y[1][34] == 0.0f);
    CHECK(y[1][35] == Approx(frame(30)).margin(1e-6));
    CHECK(y[1][44] == Approx(frame(39)).margin(1e-6));
}

TEST_CASE("samplePlayer signals the end at the next block when its last frame ends a block") {
    // 40 frames from the start end on sample 39, the last of a 10-sample block.
    const auto y = listen(player({}, 4.0), 60);
    CHECK(y[0][39] == Approx(frame(39)).margin(1e-6));
    CHECK(y[1][39] == 0.0f);
    CHECK(y[1][40] == Approx(frame(30)).margin(1e-6));
}

TEST_CASE("samplePlayer's rate sets its speed and direction") {
    // Uninterpolated, so the frames are exact: a filtered read (6.5) blurs them.
    const auto twice = listen(player({{"rate", 2.0}, {"interpolation", std::string("none")}}, 4.0), 30);
    for (std::size_t i = 0; i < 20; ++i) CHECK(twice[0][i] == Approx(frame(2.0 * asDouble(i))).margin(1e-6));
    CHECK(twice[0][20] == 0.0f);
    const auto backward = listen(player({{"rate", -1.0}, {"start", 0.039}}, 4.0), 50);
    for (std::size_t i = 0; i < 40; ++i)
        CHECK(backward[0][i] == Approx(frame(39.0 - asDouble(i))).margin(1e-6));
    CHECK(backward[0][40] == 0.0f);
}

TEST_CASE("a note-on plays at the rate of its interval from root") {
    // An octave up plays at twice the speed; an octave down at half, here
    // between frames, linearly interpolated.
    const auto up = listen(player({{"root", 60.0}, {"interpolation", std::string("none")}}, 4.0, 72.0), 10);
    for (std::size_t i = 0; i < 10; ++i) CHECK(up[0][i] == Approx(frame(2.0 * asDouble(i))).margin(1e-6));
    const auto down =
        listen(player({{"root", 60.0}, {"interpolation", std::string("linear")}}, 4.0, 48.0), 10);
    for (std::size_t i = 0; i < 10; ++i) CHECK(down[0][i] == Approx(frame(0.5 * asDouble(i))).margin(1e-6));
    // Without :root, a note plays at :rate.
    const auto plain = listen(player({}, 4.0, 72.0), 10);
    CHECK(plain[0][9] == Approx(frame(9)).margin(1e-6));
}

TEST_CASE("samplePlayer loops forward and ping-pong between loop-start and loop-end") {
    const auto forward = listen(
        player({{"loop", std::string("forward")}, {"loop-start", 0.010}, {"loop-end", 0.020}}, 4.0), 60);
    for (std::size_t i = 0; i < 20; ++i) CHECK(forward[0][i] == Approx(frame(asDouble(i))).margin(1e-6));
    for (std::size_t i = 20; i < 60; ++i)
        CHECK(forward[0][i] == Approx(frame(asDouble(10 + ((i - 20) % 10)))).margin(1e-6));
    CHECK(forward[1][59] == 0.0f);  // a loop never ends

    const auto pingPong = listen(
        player({{"loop", std::string("ping-pong")}, {"loop-start", 0.010}, {"loop-end", 0.020}}, 4.0), 60);
    // 0 to 19, back down to 10, up again: neither end plays twice.
    const std::vector<double> turn{18, 17, 16, 15, 14, 13, 12, 11, 10, 11, 12};
    for (std::size_t i = 0; i < turn.size(); ++i)
        CHECK(pingPong[0][20 + i] == Approx(frame(turn[i])).margin(1e-6));
    // Loop-end 0 is the end of the buffer.
    const auto whole = listen(player({{"loop", std::string("forward")}}, 4.0), 90);
    CHECK(whole[0][45] == Approx(frame(5)).margin(1e-6));
}

TEST_CASE("a retrigger crossfades over 2 ms from the old head to the start (samples.md 7.2)") {
    // Triggers every 32 samples; the fade is 2 samples at 1 kHz.
    const auto y = listen(player({}, 0.5), 40);
    CHECK(y[0][31] == Approx(frame(31)).margin(1e-6));
    CHECK(y[0][32] == Approx(frame(32)).margin(1e-6));  // all old head
    CHECK(y[0][33] == Approx((0.5 * frame(1)) + (0.5 * frame(33))).margin(1e-6));
    CHECK(y[0][34] == Approx(frame(2)).margin(1e-6));  // all new head
}

TEST_CASE("a samplePlayer that has not been triggered is silent") {
    auto g = player({}, 4.0);
    g.spec.nodes[1].inputs["trigger"] = {support::events(3)};  // a pipe nothing writes
    const auto y = listen(g, 30);
    CHECK(std::ranges::all_of(y[0], [](float v) { return v == 0.0f; }));
}

TEST_CASE("a swap keeps a samplePlayer's head") {
    rig r;
    const auto g = player({}, 4.0).spec;
    const auto p = r.play(g);
    for (std::size_t k = 0; k < 40; ++k) p->buffers[0]->channel(0)[k] = static_cast<float>(k) / 100.0f;
    r.run(15);
    r.play(g, 2);
    r.run(10);
    for (std::size_t i = 15; i < 25; ++i) CHECK(r.out(0)[i] == Approx(frame(asDouble(i))).margin(1e-6));
}

namespace {

// grains over a 1000-frame ring whose frame k holds @p fill(k), to output
// 0, or outputs 0 and 1 when @p stereo. A metro every @p every beats at
// 937.5 BPM (64 samples a beat) triggers grains when given.
support::graph granular(std::map<std::string, metadata::paramValue, std::less<>> params, bool stereo = false,
                        std::optional<double> every = std::nullopt) {
    support::graph g;
    g.spec.tempo = 937.5;
    g.spec.buffers = {ring(1000)};
    g.pipe(support::events(2)).pipe(audio(9), stereo ? 2 : 1);
    if (every) {
        auto& clock = g.node("clock", "metro");
        clock.params["every"] = *every;
        clock.outputs["out"] = {support::events(2)};
    }
    auto& gr = g.node("grains", "grains");
    gr.params["buffer"] = metadata::bufferRef{.index = 0};
    for (auto& [name, value] : params) gr.params[name] = value;
    if (every) gr.inputs["trigger"] = {support::events(2)};
    gr.outputs["out"] = {audio(9)};
    g.node("out", "audioOut").inputs["in"] = {audio(9)};
    return g;
}

std::array<std::vector<float>, 2> grainsOf(const support::graph& g, int n,
                                           const std::function<float(std::size_t)>& fill) {
    rig r(silence, fill);
    r.play(filled(g).spec);
    r.run(n);
    return {r.out(0), r.out(1)};
}

float constant(std::size_t) { return 0.5f; }
float ramp(std::size_t k) { return static_cast<float>(k) / 1000.0f; }

double hann(double x) { return 0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * x)); }

}  // namespace

TEST_CASE("a grain is its window over the buffer it reads (samples.md 8.1)") {
    // One grain at a trigger, 10 samples long.
    const auto y = grainsOf(granular({{"density", 0.0}, {"duration", 0.010}}, false, 4.0), 30, constant);
    for (std::size_t k = 0; k < 10; ++k)
        CHECK(y[0][k] == Approx(0.5 * hann(asDouble(k) / 10.0)).margin(1e-6));
    for (std::size_t k = 10; k < 30; ++k) CHECK(y[0][k] == 0.0f);
    const auto tri = grainsOf(
        granular({{"density", 0.0}, {"duration", 0.010}, {"window", std::string("triangle")}}, false, 4.0),
        10, constant);
    CHECK(tri[0][5] == Approx(0.5).margin(1e-6));
    CHECK(tri[0][2] == Approx(0.2).margin(1e-6));
    const auto tukey = grainsOf(
        granular({{"density", 0.0}, {"duration", 0.010}, {"window", std::string("tukey")}}, false, 4.0), 10,
        constant);
    CHECK(tukey[0][4] == Approx(0.5).margin(1e-6));  // flat in the middle half
    CHECK(tukey[0][1] < 0.5f);
}

TEST_CASE("a grain reads from its position at its pitch forward or backward") {
    // From frame 100, an octave up: frames 100, 102, 104, ..., band-limited
    // (6.5), which keeps a ramp to within 0.1%.
    const auto up = grainsOf(
        granular({{"density", 0.0}, {"duration", 0.010}, {"position", 0.1}, {"pitch", 12.0}}, false, 4.0), 10,
        ramp);
    for (std::size_t k = 1; k < 10; ++k) {
        CHECK(up[0][k] ==
              Approx(hann(asDouble(k) / 10.0) * (100.0 + (2.0 * asDouble(k))) / 1000.0).margin(1e-4));
    }
    const auto back = grainsOf(
        granular({{"density", 0.0}, {"duration", 0.010}, {"position", 0.1}, {"reverse", 1.0}}, false, 4.0),
        10, ramp);
    for (std::size_t k = 1; k < 10; ++k) {
        CHECK(back[0][k] == Approx(hann(asDouble(k) / 10.0) * (100.0 - asDouble(k)) / 1000.0).margin(1e-6));
    }
}

TEST_CASE("density starts grains at its rate and none at 0 without a trigger") {
    // 10 ms grains, each summing to 5 times the buffer's value.
    const auto fifty = grainsOf(granular({{"density", 50.0}, {"duration", 0.010}}), 1000, constant);
    double sum = 0.0;
    for (const float v : fifty[0]) sum += v;
    CHECK(sum == Approx(50 * 5 * 0.5).margin(5 * 0.5));
    const auto none = grainsOf(granular({{"density", 0.0}}), 200, constant);
    CHECK(std::ranges::all_of(none[0], [](float v) { return v == 0.0f; }));
}

TEST_CASE("jitter moves each grain's start up to its amount and pitch-jitter its pitch either way") {
    // Two-sample grains: the second sample has a window of 1 and reads one
    // step past the start, from frame 100 plus up to 50 frames of jitter.
    const auto y =
        grainsOf(granular({{"density", 100.0}, {"duration", 0.002}, {"position", 0.1}, {"jitter", 0.050}}),
                 1000, ramp);
    std::size_t heard = 0;
    for (const float v : y[0]) {
        if (v == 0.0f) continue;
        ++heard;
        CHECK(v >= 0.1009f);
        CHECK(v <= 0.1511f);
    }
    CHECK(heard == 100);
    // Up to an octave either way: a step from 0.5 to 2 frames.
    const auto bent = grainsOf(
        granular({{"density", 100.0}, {"duration", 0.002}, {"position", 0.1}, {"pitch-jitter", 12.0}}), 1000,
        ramp);
    float low = 1.0f;
    float high = 0.0f;
    for (const float v : bent[0]) {
        if (v == 0.0f) continue;
        low = std::min(low, v);
        high = std::max(high, v);
    }
    CHECK(low >= 0.1005f - 1e-6f);
    CHECK(high <= 0.102f + 1e-6f);
    CHECK(high - low > 0.001f);  // they do vary
}

TEST_CASE(
    "a grain started with every voice busy replaces the oldest which fades over 2 ms (samples.md 8.2)") {
    // One voice; 20-sample grains every 5 samples; a flat-topped window.
    auto g =
        granular({{"density", 0.0}, {"duration", 0.020}, {"voices", 1.0}, {"window", std::string("tukey")}},
                 false, 5.0 / 64.0);
    // A buffer of 0.1, so four grains summed stay under the output limiter.
    const auto tenth = [](std::size_t) { return 0.1f; };
    const auto y = grainsOf(g, 40, tenth);
    const auto tukeyAt = [](double x) {
        const double edge = std::fmin(x, 1.0 - x);
        return edge >= 0.25 ? 1.0 : hann(edge / 0.5);
    };
    // At sample 12, the grain of sample 10 is two samples old and the one
    // it replaced has faded: only the new grain sounds.
    CHECK(y[0][12] == Approx(0.1 * tukeyAt(2.0 / 20.0)).margin(1e-6));
    CHECK(*std::ranges::max_element(y[0]) <= 0.2f + 1e-6f);  // a grain and a fading one, never four
    // With 32 voices the grains overlap: at sample 30, those of samples 15,
    // 20, and 25 are at the top of their windows, and the new one at 0.
    g.spec.nodes[1].params["voices"] = 32.0;
    const auto many = grainsOf(g, 40, tenth);
    CHECK(many[0][30] == Approx(0.3).margin(1e-6));
}

TEST_CASE("spread pans grains at equal power and grains repeat for one seed (samples.md 8.3)") {
    auto g = granular({{"density", 200.0}, {"duration", 0.002}, {"spread", 1.0}}, true);
    const auto y = grainsOf(g, 1000, constant);
    bool panned = false;
    for (std::size_t i = 0; i < 1000; ++i) {
        const double power = (y[0][i] * y[0][i]) + (y[1][i] * y[1][i]);
        if (power == 0.0) continue;
        CHECK(power == Approx(0.25).margin(1e-5));  // a window of 1 on 0.5, split between the two
        panned = panned || std::abs(y[0][i] - y[1][i]) > 0.05f;
    }
    CHECK(panned);
    CHECK(support::bitDifference(grainsOf(g, 1000, constant), y) == "");
    g.spec.seed = 1;
    CHECK(grainsOf(g, 1000, constant) != y);
}

TEST_CASE("a swap keeps the grains that sound") {
    const auto g = granular({{"density", 50.0}, {"duration", 0.030}, {"jitter", 0.1}}).spec;
    rig steady;
    rig swapped;
    for (rig& r : {std::ref(steady), std::ref(swapped)}) {
        const auto p = r.play(g);
        for (std::size_t k = 0; k < 1000; ++k) p->buffers[0]->channel(0)[k] = ramp(k);
    }
    steady.run(200);
    swapped.run(100);
    swapped.play(g, 2);
    swapped.run(100);
    CHECK(swapped.out(0) == steady.out(0));
}

TEST_CASE("readers of a buffer at its file's own rate play it at that rate (samples.md 4.5.1)") {
    // kept.wav holds 2 frames a millisecond: 5 ms is frame 10, and rate 1
    // plays 2 frames a sample.
    const auto played = listen(
        keptAt(player({{"start", 0.005}, {"interpolation", std::string("none")}}, 4.0), "kept.wav"), 20);
    for (std::size_t i = 0; i < 15; ++i)
        CHECK(played[0][i] == Approx(frame(10.0 + (2.0 * asDouble(i)))).margin(1e-6));
    CHECK(played[0][15] == 0.0f);

    support::graph g;
    g.spec.buffers = {ring(40)};
    addReader(g, 0.005, "none");
    rig r;
    const auto p = r.play(keptAt(g, "kept.wav").spec);
    for (std::size_t k = 0; k < 40; ++k) p->buffers[0]->channel(0)[k] = frame(asDouble(k));
    r.run(10);
    CHECK(r.out(0)[9] == frame(10));

    // A grain from 50 ms, frame 100, reads 2 frames a sample, band-limited,
    // and still lasts 10 ms.
    const auto grain = grainsOf(
        keptAt(granular({{"density", 0.0}, {"duration", 0.010}, {"position", 0.05}}, false, 4.0), "fill.wav"),
        20, ramp);
    for (std::size_t k = 1; k < 10; ++k) {
        CHECK(grain[0][k] ==
              Approx(hann(asDouble(k) / 10.0) * (100.0 + (2.0 * asDouble(k))) / 1000.0).margin(1e-4));
    }
    for (std::size_t k = 10; k < 20; ++k) CHECK(grain[0][k] == 0.0f);
}

namespace {

// The buffer of @p file once a writer has recorded a sine of @p hz, at
// 1 kHz, into it for @p n samples.
std::vector<float> recordSine(const std::string& file, double hz, int n) {
    rig r([hz](std::int64_t k) {
        return static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(k) / rate));
    });
    const auto p = r.play(keptAt(recorder(ring(40)), file).spec);
    r.run(n);
    return contents(*p->buffers[0]);
}

}  // namespace

TEST_CASE(
    "bufferWrite resamples into a buffer at its file's own rate, band-limited (samples.md 4.5.1, 6.5)") {
    // Into half the rate, whose Nyquist frequency is 250 Hz: 50 Hz passes,
    // and 400 Hz, which would fold to 100 Hz, does not. Frames 200 to 999
    // hold 80 cycles of 50 Hz and 160 of 100.
    const auto low = recordSine("half.wav", 50.0, 2100);
    CHECK(support::amplitude(std::span(low).subspan(200, 800), 0.1) == Approx(1.0).margin(0.01));
    const auto high = recordSine("half.wav", 400.0, 2100);
    CHECK(support::amplitude(std::span(high).subspan(200, 800), 0.2) < 5e-4);  // over 65 dB down
    // Into twice the rate: 300 Hz passes, and its image at 700 Hz is gone.
    // Frames 400 to 1999 hold 240 and 560 cycles.
    const auto up = recordSine("twice.wav", 300.0, 1050);
    const auto heard = std::span(up).subspan(400, 1600);
    CHECK(support::amplitude(heard, 0.15) == Approx(1.0).margin(0.01));
    CHECK(support::amplitude(heard, 0.35) < 5e-4);
}

namespace {

// A sine of @p cycles a frame, frame by frame.
std::function<float(std::size_t)> sine(double cycles) {
    return [cycles](std::size_t k) {
        return static_cast<float>(std::sin(2.0 * std::numbers::pi * cycles * static_cast<double>(k)));
    };
}

// What a samplePlayer at @p speed, reading with @p mode, plays of a sine
// of @p cycles a frame filling @p frames frames: the amplitude at @p heard
// cycles a sample over samples 100 to 399. With @p written, a writer
// recording nothing is bound to the buffer.
struct sineRead {
    double speed = 1.0;
    std::string mode = "cubic";
    double frames = 1000.0;
    bool written = false;

    double heard(double cycles, double at) const {
        auto g = player({{"rate", speed}, {"interpolation", mode}}, 1000.0);
        g.spec.buffers = {ring(frames)};
        if (written) {
            auto& w = g.node("write", "bufferWrite");
            w.params["buffer"] = metadata::bufferRef{.index = 0};
            w.params["record"] = 0.0;
            w.inputs["in"] = {audio(9)};
        }
        rig r(silence, sine(cycles));
        r.play(filled(g).spec);
        r.run(400);
        return support::amplitude(std::span(r.out(0)).subspan(100, 300), at);
    }
};

}  // namespace

TEST_CASE("the Kaiser window's Bessel function sums its series to the tolerance given (samples.md 6.5)") {
    using primitives::besselI0;
    static_assert(besselI0(0.0, 1e-12) == 1.0);
    // I0(1) and I0(6), the kernel's shape, summed exactly in rationals.
    CHECK(besselI0(1.0, 1e-17) == Approx(1.2660658777520084).epsilon(1e-15));
    CHECK(besselI0(6.0, 1e-17) == Approx(67.23440697647797).epsilon(1e-15));
    CHECK(besselI0(6.0, 1e-12) == Approx(67.23440697647797).epsilon(1e-11));
    // Terms are added while the last added exceeds the tolerance times the
    // sum: at x = 1, the second term, 0.25, is not past half of 1.25, and
    // the first, 1, is not past all of itself.
    CHECK(besselI0(1.0, 0.5) == 1.25);
    CHECK(besselI0(1.0, 1.0) == 1.0);
}

TEST_CASE("reads that step faster than a frame a sample are band-limited (samples.md 6.5)") {
    // At rate 2, a sine of 0.1 cycles a frame plays at 0.2 a sample; one
    // of 0.3, past half the buffer's Nyquist frequency, would fold to 0.4.
    // Samples 100 to 399 hold 60 and 120 cycles.
    for (const std::string mode : {"cubic", "linear"}) {
        INFO(mode);
        const sineRead read{.speed = 2.0, .mode = mode, .frames = 1000.0, .written = false};
        CHECK(read.heard(0.1, 0.2) == Approx(1.0).margin(0.01));
        CHECK(read.heard(0.3, 0.4) < 1e-3);  // over 60 dB down
    }
    // Uninterpolated reads are not filtered.
    CHECK(sineRead{.speed = 2.0, .mode = "none", .frames = 1000.0, .written = false}.heard(0.3, 0.4) > 0.9);

    // A grain an octave up over the folding sine is silent.
    const auto grain = grainsOf(
        granular({{"density", 0.0}, {"duration", 0.5}, {"position", 0.1}, {"pitch", 12.0}}, false, 4.0), 500,
        sine(0.3));
    CHECK(support::peak(grain[0]) < 1e-3);
}

TEST_CASE("fast reads of a buffer no writer records into read its levels (samples.md 6.6)") {
    // At rate 16, past the filter's width of 8: 0.01 cycles a frame plays
    // at 0.16 a sample; 0.04, which would fold to 0.36, is gone, as level 4
    // holds nothing above 0.88 of its Nyquist frequency.
    const sineRead fast{.speed = 16.0, .mode = "cubic", .frames = 16000.0, .written = false};
    CHECK(fast.heard(0.01, 0.16) == Approx(1.0).margin(0.01));
    CHECK(fast.heard(0.04, 0.36) < 1e-3);
    // A buffer a writer is bound to has no levels: past a speed of 8 the
    // filter stays at 8's width, and 0.04 a frame folds.
    CHECK(sineRead{.speed = 16.0, .mode = "cubic", .frames = 16000.0, .written = true}.heard(0.04, 0.36) >
          0.5);
}

TEST_CASE(
    "levels count against buffer memory, and a plan they do not fit goes without them (samples.md 6.6)") {
    const auto build = [](std::size_t memory) {
        auto g = player({}, 1000.0);
        g.spec.buffers = {ring(16000)};
        primitives::engine e({.audio = {.sampleRate = rate, .blockSize = block},
                              .pipes = {},
                              .loadImage = nullptr,
                              .loadSample = loaderOf(sine(0.01)),
                              .bufferMemory = memory});
        const auto p = e.build(filled(g).spec, 1);
        return p->buffers[0]->bytes() + p->buffers[0]->levelBytes();
    };
    constexpr std::size_t samples = 16000 * sizeof(float);
    // Levels of 8000, 4000, ... 63 frames: just under the buffer again.
    const auto with = build(primitives::bufferPool::defaultLimit);
    CHECK(with > samples);
    CHECK(with < 2 * samples);
    CHECK(build(samples + 1000) == samples);
}

TEST_CASE("a refused plan takes back the marks it set on live buffers (samples.md 6.6)") {
    primitives::engine e({.audio = {.sampleRate = rate, .blockSize = block},
                          .pipes = {},
                          .loadImage = nullptr,
                          .loadSample = loaderOf(sine(0.01)),
                          .bufferMemory = primitives::bufferPool::defaultLimit});
    auto g = player({}, 1000.0);
    g.spec.buffers = {ring(16000)};
    g = filled(g);
    const auto first = e.build(g.spec, 1);
    const auto& buffer = *first->buffers[0];
    REQUIRE(buffer.levelBytes() > 0);
    // A writer of the buffer, and a node the engine cannot make.
    auto refused = g;
    auto& w = refused.node("write", "bufferWrite");
    w.params["buffer"] = metadata::bufferRef{.index = 0};
    w.inputs["in"] = {audio(9)};
    refused.node("odd", "noSuchPrimitive");
    CHECK_THROWS(e.build(refused.spec, 2));
    CHECK_FALSE(buffer.written());
    // The next plan adopts the buffer and reads it through levels again.
    const auto later = e.build(g.spec, 3);
    CHECK(later->buffers[0] == first->buffers[0]);
    CHECK(buffer.levelBytes() > 0);
}
