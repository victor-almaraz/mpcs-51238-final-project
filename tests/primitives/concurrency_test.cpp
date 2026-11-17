// The swap handoff under concurrency: one thread plays plans while
// another processes audio, as the server's control and audio threads do.

#include <catch2/catch_test_macros.hpp>

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

metadata::graphSpec sine(int variant) {
    graph g;
    g.pipe(audio(0));
    auto& s = g.node("sine" + std::to_string(variant % 2), "sineOsc");  // alternate adoption and crossfades
    s.params["freq"] = 220.0 + variant % 7;
    s.outputs["out"] = {audio(0)};
    g.node("out", "audioOut").inputs["in"] = {audio(0)};
    return g.spec;
}

} // namespace

TEST_CASE("plans played while audio runs are each activated and retired once") {
    engine e({.audio = {.sampleRate = 48000.0, .blockSize = 64}, .pipes = {}});
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
    constexpr int plans = 3000;
    for (int i = 1; i <= plans; ++i) {
        const bool last = i == plans;
        const auto quant = !last && i % 5 == 0 ? quantization::beat : quantization::none;
        e.play(e.build(sine(i), i), last || i % 3 == 0 ? 0.0 : 0.001, quant);
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

// A background of one of two colors, shown by the visual layer.
metadata::graphSpec shownColor(int variant) {
    graph g;
    g.pipe(audio(0));
    g.pipe(control(1));
    g.pipe(video(0));
    g.pipe(video(1));
    // A rect, transparent, whose position follows an audio pipe and a
    // control pipe: the render thread reads both while the audio thread
    // writes them.
    auto& wave = g.node("wave", "sineOsc");
    wave.params["freq"] = 440.0;
    wave.outputs["out"] = {audio(0)};
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
    engine e({.audio = {.sampleRate = 48000.0, .blockSize = 64, .frameWidth = 16, .frameHeight = 8},
              .pipes = {.video = 2}});
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

    constexpr int plays = 2000;
    for (int i = 1; i <= plays; ++i) {
        e.play(e.build(shownColor(i), i), 0.0, quantization::none);
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
