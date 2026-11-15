// The swap handoff under concurrency: one thread plays plans while
// another processes audio, as the server's control and audio threads do.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <thread>

#include <proj/protocol/jthread.h>

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
    engine e({{48000.0, 64}, {}});
    proj::jthread audio([&](const proj::stop_token& stop) {
        std::vector<float> out(64);
        const std::span<float> outs[] = {std::span(out)};
        while (!stop.stop_requested()) e.process({{}, outs}, 64, 0.0);
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
