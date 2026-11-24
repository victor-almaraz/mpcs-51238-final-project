// The swap handoff under concurrency: one thread plays plans while
// another processes audio, as the server's control and audio threads do,
// on the audio thread alone and with workers (threads.md §9).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <thread>
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

