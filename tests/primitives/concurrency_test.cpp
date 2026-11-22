// The swap handoff under concurrency: one thread plays plans while
// another processes audio, as the server's control and audio threads do,
// on the audio thread alone and with workers (threads.md §9).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/engine.h>
#include <espresso/protocol/jthread.h>

#include "support/graph.h"

using namespace support;
using namespace primitives;

namespace {

// How a test runs the engine, in blocks of 64 samples, and how many plans
// it plays: on the audio thread alone; or with three workers, with a
// second sine in each plan. With a least task cost of 0 each sine is a
// task of its own, so the workers run one. The run with workers plays
// fewer plans, as on a machine of few cores the spinning workers slow
// this thread.
struct engineRun {
    int threads = 1;
    int plays = 0;

    // The sines in each plan.
    int sines() const { return threads > 1 ? 2 : 1; }
};

// @p count sines, the first to the output. Their IDs alternate between
// two sets, so swaps alternate between adoption and crossfades.
metadata::graphSpec sines(int variant, int count) {
    graph g;
    for (int k = 0; k < count; ++k) {
        g.pipe(audio(k));
        auto& s = g.node("sine" + std::to_string(2 * (variant % 2) + k), "sineOsc");
        s.params["freq"] = 220.0 * (k + 1) + variant % 7;
        s.outputs["out"] = {audio(k)};
    }
    g.node("out", "audioOut").inputs["in"] = {audio(0)};
    return g.spec;
}

} // namespace

TEST_CASE("plans played while audio runs are each activated and retired once") {
    const auto run = GENERATE(engineRun{.threads = 1, .plays = 3000}, engineRun{.threads = 4, .plays = 200});
    INFO(run.threads << " threads");
    engine e({.audio = {.sampleRate = 48000.0, .blockSize = 64},
              .pipes = {.audio = 4},
              .threads = run.threads,
              .schedule = support::finest});
    if (run.threads > 1) REQUIRE(e.build(sines(0, run.sines()), 0)->schedule.tasks.parallel);
    espresso::jthread audio([&](const espresso::stop_token& stop) {
        std::vector<float> out(64);
        const std::array outs{std::span(out)};
        while (!stop.stop_requested()) e.process({.inputs = {}, .outputs = outs}, 64, 0.0);
    });

    std::map<std::int32_t, int> activated, retired;
    std::int32_t lastActivated = 0;
    std::vector<std::int32_t> outOfOrder;    // activated after a later plan
    std::vector<std::int32_t> retiredEarly;  // retired while still the latest activated
    std::vector<std::int32_t> revived;       // activated after it was retired
    auto record = [&] {
        for (const auto& n : e.collect()) {
            if (n.what == notice::kind::activated) {
                if (n.plan <= lastActivated) outOfOrder.push_back(n.plan);
                if (retired[n.plan] > 0) revived.push_back(n.plan);
                lastActivated = std::max(lastActivated, n.plan);
                ++activated[n.plan];
            } else {
                // A plan that ran is retired only after a later plan
                // activated; one withdrawn before it ran never activates.
                if (activated[n.plan] > 0 && lastActivated <= n.plan) retiredEarly.push_back(n.plan);
                ++retired[n.plan];
            }
        }
    };
    const int plans = run.plays;
    for (int i = 1; i <= plans; ++i) {
        const bool last = i == plans;
        const auto quant = !last && i % 5 == 0 ? quantization::beat : quantization::none;
        e.play(e.build(sines(i, run.sines()), i), last || i % 3 == 0 ? 0.0 : 0.001, quant);
        record();
        if (i % 11 == 0) std::this_thread::yield();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (e.counters().activePlan != plans && std::chrono::steady_clock::now() < deadline) {
        record();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    audio.request_stop();
    audio.join();
    record();

    CHECK(e.counters().activePlan == plans);
    for (int i = 1; i <= plans; ++i) {
        INFO("plan " << i);
        CHECK(activated[i] <= 1);
        // Every plan but the last is gone: withdrawn before it ran, or
        // retired after it ran.
        CHECK(retired[i] == (i == plans ? 0 : 1));
    }
    CHECK(activated[plans] == 1);
    CHECK(outOfOrder.empty());
    CHECK(retiredEarly.empty());
    CHECK(revived.empty());
}

namespace {

// A background of one of two colors, shown by the visual layer, and
// @p sines sines.
metadata::graphSpec shownColor(int variant, int sines) {
    graph g;
    g.pipe(audio(0));
    g.pipe(audio(1));
    g.pipe(control(1));
    g.pipe(video(0));
    g.pipe(video(1));
    // A rect, transparent, whose position follows an audio pipe and a
    // control pipe: the render thread reads both while the audio thread
    // or a worker writes them.
    for (int k = 0; k < sines; ++k) {
        auto& wave = g.node("wave" + std::to_string(k), "sineOsc");
        wave.params["freq"] = 440.0;
        wave.outputs["out"] = {audio(k)};
    }
    auto& bg = g.node("bg", "background");
    bg.params["color"] =
        variant % 2 == 0 ? std::vector<double>{1.0, 0.0, 0.0, 1.0} : std::vector<double>{0.0, 0.0, 1.0, 1.0};
    bg.outputs["out"] = {video(0)};
    auto& box = g.node("box", "rect");
    box.params = {{"x", audio(0)},
                  {"y", control(1)},
                  {"fill", std::vector<double>{0.0, 1.0, 0.0, 0.0}},
                  {"stroke", std::string("none")}};
    box.inputs["in"] = {video(0)};
    box.outputs["out"] = {video(1)};
    g.node("show", "videoOut").inputs["in"] = {video(1)};
    return g.spec;
}

}  // namespace

TEST_CASE("the render thread shows only whole frames of activated plans while plans swap") {
    const auto run = GENERATE(engineRun{.threads = 1, .plays = 2000}, engineRun{.threads = 4, .plays = 160});
    INFO(run.threads << " threads");
    engine e({.audio = {.sampleRate = 48000.0, .blockSize = 64, .frameWidth = 16, .frameHeight = 8},
              .pipes = {.audio = 4, .video = 2},
              .threads = run.threads,
              .schedule = support::finest});
    if (run.threads > 1) REQUIRE(e.build(shownColor(0, run.sines()), 0)->schedule.tasks.parallel);
    espresso::jthread audio([&](const espresso::stop_token& stop) {
        while (!stop.stop_requested()) e.process({}, 64, 0.0);
    });
    std::atomic<int> frames{0};
    std::atomic<int> torn{0};  // frames with more than one color, or another color
    espresso::jthread render([&](const espresso::stop_token& stop) {
        for (double t = 0.0; !stop.stop_requested(); t += 1.0 / 60.0) {
            const auto f = e.renderFrame(t);
            if (!f) continue;
            const auto pixels = f->get().pixels();
            const std::uint32_t first = pixels.front();
            const bool whole = std::ranges::all_of(pixels, [&](std::uint32_t p) { return p == first; });
            if (!whole || (first != 0xffff0000u && first != 0xff0000ffu)) ++torn;
            ++frames;
        }
    });

    for (int i = 1; i <= run.plays; ++i) {
        e.play(e.build(shownColor(i, run.sines()), i), 0.0, quantization::none);
        std::ignore = e.setControl(1, static_cast<float>(i % 7));
        e.collect();
    }
    // Wait until the render thread has drawn a frame.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (frames.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        e.collect();
        std::this_thread::yield();
    }
    render.request_stop();
    render.join();
    audio.request_stop();
    audio.join();
    e.collect();
    CHECK(frames.load() > 0);
    CHECK(torn.load() == 0);
}
