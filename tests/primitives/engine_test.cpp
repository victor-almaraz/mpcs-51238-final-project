// Plan swaps: adoption, crossfades, quantization, stop, and determinism
// (primitives.md §9, §11.2).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/plan.h>
#include <espresso/primitives/primitive.h>

#include "support/graph.h"

using Catch::Approx;
using namespace support;
using namespace primitives;

namespace {

// A sine with node ID @p id through device output 0.
metadata::graphSpec sine(double freq, const std::string& id = "sine") {
    graph g;
    g.pipe(audio(0));
    auto& s = g.node(id, "sineOsc");
    s.params["freq"] = freq;
    s.outputs["out"] = {audio(0)};
    g.node("out", "audioOut").inputs["in"] = {audio(0)};
    return g.spec;
}

bool has(const std::vector<notice>& ns, notice::kind k, std::int32_t plan) {
    return std::ranges::any_of(ns, [&](const notice& n) { return n.what == k && n.plan == plan; });
}

} // namespace

TEST_CASE("a matching node adopts state: the swap is seamless") {
    renderer continuous(1);
    continuous.play(sine(440.0), 1);
    continuous.run(9600);

    renderer swapped(1);
    swapped.play(sine(440.0), 1);
    swapped.run(4800);
    swapped.play(sine(440.0), 2, 0.05);  // same IDs, types, shapes: no fade
    swapped.run(4800);
    CHECK(swapped.out() == continuous.out());

    const auto ns = swapped.engine.collect();
    CHECK(has(ns, notice::kind::activated, 1));
    CHECK(has(ns, notice::kind::activated, 2));
    CHECK(has(ns, notice::kind::retired, 1));
    CHECK(swapped.engine.counters().activePlan == 2);
}

TEST_CASE("a changed node crossfades for the fade period") {
    renderer r(1);
    r.play(sine(440.0, "a"), 1);
    r.run(4800);
    r.play(sine(440.0, "b"), 2, 0.1);  // a new ID: a fades out, b fades in
    r.run(64);
    CHECK_FALSE(has(r.engine.collect(), notice::kind::retired, 1));
    r.run(4800);
    CHECK(has(r.engine.collect(), notice::kind::retired, 1));
    // Both sines start at phase 0 at different times; during the fade the
    // sum stays within the sum of the gains, and afterwards only b plays.
    CHECK(peak(r.out(), 4800) <= 1.0 + 1e-5);
    CHECK(rms(r.out(), 4800 + 4800 + 64 - 2400) == Approx(std::sqrt(0.5)).epsilon(0.02));
}

TEST_CASE("first play fades in; stop fades out and retires") {
    renderer r(1);
    r.play(sine(1000.0), 1, 0.01);
    r.run(4800);
    CHECK(peak(r.out(), 0, 48) < 0.1);  // the first millisecond is quiet
    CHECK(peak(r.out(), 480, 4800) > 0.99);
    r.engine.stop(0.01);
    r.run(4800);
    CHECK(peak(r.out(), 4800 + 512) == 0.0);  // the fade ends at the block after 480 samples
    const auto ns = r.engine.collect();
    CHECK(has(ns, notice::kind::retired, 1));
    CHECK(r.engine.counters().activePlan == 0);
}

TEST_CASE("quantized swaps wait for the beat or bar") {
    renderer r(1, 48000.0, 100);
    r.play(sine(440.0, "a"), 1);
    r.run(1000);
    r.play(sine(440.0, "b"), 2, 0.0, quantization::beat);  // beat 1 is sample 24000
    r.run(23000);
    CHECK(r.engine.counters().activePlan == 1);
    r.run(1100);
    CHECK(r.engine.counters().activePlan == 2);

    r.play(sine(440.0, "c"), 3, 0.0, quantization::bar);  // bar 1 is beat 4: sample 96000
    r.run(96000 - 25100 - 100);
    CHECK(r.engine.counters().activePlan == 2);
    r.run(200);
    CHECK(r.engine.counters().activePlan == 3);
}

TEST_CASE("a newer play replaces a swap still waiting for its target") {
    renderer r(1);
    r.play(sine(440.0, "a"), 1);
    r.run(64);
    r.play(sine(440.0, "b"), 2, 0.0, quantization::bar);
    r.play(sine(440.0, "c"), 3);
    r.run(64);
    const auto ns = r.engine.collect();
    CHECK(has(ns, notice::kind::retired, 2));  // withdrawn before it ran
    CHECK_FALSE(has(ns, notice::kind::activated, 2));
    CHECK(has(ns, notice::kind::activated, 3));
    CHECK(has(ns, notice::kind::retired, 1));
}

TEST_CASE("a swap during a fade waits for the fade to finish") {
    renderer r(1);
    r.play(sine(440.0, "a"), 1);
    r.run(64);
    r.play(sine(440.0, "b"), 2, 0.1);  // 4800 samples
    r.run(64);
    r.play(sine(440.0, "c"), 3, 0.01);
    r.run(2400);
    CHECK(r.engine.counters().activePlan == 2);  // c waits
    r.run(4800);
    const auto ns = r.engine.collect();
    CHECK(has(ns, notice::kind::retired, 1));
    CHECK(has(ns, notice::kind::retired, 2));
    CHECK(r.engine.counters().activePlan == 3);
    CHECK(peak(r.out(), std::size_t{64} * 2) <= 1.0 + 1e-5);
}

TEST_CASE("a crossfade between separate chains keeps the level") {
    // A constant through gain into audioOut, with different IDs and pipes
    // in each plan: each chain is scaled once, so the sum stays at 0.5.
    auto chain = [](const std::string& id, int base) {
        graph g;
        g.pipe(audio(base)).pipe(audio(base + 1));
        auto& level = g.node(id + "-level", "lfo");
        level.params = {{"depth", 0.0}, {"offset", 1.0}};
        level.outputs["out"] = {audio(base)};
        auto& gain = g.node(id + "-gain", "gain");
        gain.params["amount"] = 0.5;
        gain.inputs["in"] = {audio(base)};
        gain.outputs["out"] = {audio(base + 1)};
        g.node(id + "-out", "audioOut").inputs["in"] = {audio(base + 1)};
        return g.spec;
    };
    renderer r(1);
    r.play(chain("a", 0), 1);
    r.run(640);
    r.play(chain("b", 2), 2, 0.05);
    r.run(4800);
    for (std::size_t i = 640; i < r.out().size(); ++i) {
        INFO(i);
        REQUIRE(r.out()[i] == Approx(0.5f).margin(1e-4));
    }
}

TEST_CASE("the same graph and seed render identically") {
    graph g;
    g.pipe(audio(0)).pipe(audio(1)).pipe(control(1));
    auto& lfo = g.node("lfo", "lfo");
    lfo.params = {{"shape", std::string("sample-and-hold")}, {"rate", 8.0}, {"depth", 400.0}, {"offset", 600.0}};
    lfo.outputs["out"] = {control(1)};
    auto& n = g.node("noise", "noise");
    n.outputs["out"] = {audio(0)};
    auto& f = g.node("filter", "svf");
    f.params = {{"cutoff", control(1)}, {"mode", std::string("band")}};
    f.inputs["in"] = {audio(0)};
    f.outputs["out"] = {audio(1)};
    g.node("out", "audioOut").inputs["in"] = {audio(1)};
    g.spec.seed = 42;
    renderer a(1), b(1);
    a.play(g.spec);
    b.play(g.spec);
    a.run(48000);
    b.run(48000);
    CHECK(a.out() == b.out());
    CHECK(rms(a.out()) > 0.01);
}

TEST_CASE("a feedback pipe closes a loop one block late") {
    graph g;
    g.pipe(audio(0), 1, true).pipe(audio(1)).pipe(audio(2));
    g.node("in", "audioIn").outputs["out"] = {audio(1)};
    auto& sum = g.node("sum", "add");
    sum.inputs["in"] = {audio(1), audio(0)};
    sum.outputs["out"] = {audio(2)};
    auto& half = g.node("half", "gain");
    half.params["amount"] = 0.5;
    half.inputs["in"] = {audio(2)};
    half.outputs["out"] = {audio(0)};
    g.node("out", "audioOut").inputs["in"] = {audio(2)};
    renderer r(1, 48000.0, 64);
    r.play(g.spec);
    std::vector<std::vector<float>> impulse(1, std::vector<float>(256, 0.0f));
    impulse[0][3] = 1.0f;
    r.run(256, impulse);
    CHECK(r.out()[3] == 1.0f);
    CHECK(r.out()[64 + 3] == 0.5f);
    CHECK(r.out()[128 + 3] == 0.25f);
}

TEST_CASE("build errors name the node") {
    engine e({.audio = {.sampleRate = 48000.0, .blockSize = 64},
              .pipes = {.audio = 4, .control = 4, .event = 4, .eventsPerPipe = 16, .poolBytes = 256}});
    auto g = sine(440.0);
    g.pipes[0].pipe.index = 10;
    g.nodes[0].outputs["out"] = {audio(10)};
    g.nodes[1].inputs["in"] = {audio(10)};
    CHECK_THROWS_WITH(e.build(g, 1), Catch::Matchers::ContainsSubstring("beyond the engine's pipe table"));

    graph p;
    p.pipe(audio(0));
    p.node("sine", "sineOsc").outputs["out"] = {audio(0)};
    auto& pan = p.node("pan", "pan");
    pan.inputs["in"] = {audio(0)};
    pan.outputs["out"] = {audio(0)};
    try {
        e.build(p.spec, 2);
        FAIL("expected a build error");
    } catch (const buildError& err) {
        CHECK(err.node() == "pan");
        CHECK_THAT(err.what(), Catch::Matchers::ContainsSubstring("at least 2 channels"));
    }
}

TEST_CASE("the tempo pipe drives the beat clock and IR tempo sets it") {
    auto g = sine(440.0);
    g.tempo = 60.0;
    renderer r(1, 48000.0, 64);
    r.play(g);
    r.run(48000);
    CHECK(r.engine.beat() == Approx(1.0).margin(1e-6));
    r.engine.setControl(metadata::tempoPipe, 240.0f);
    r.run(48000);
    CHECK(r.engine.beat() == Approx(5.0).margin(1e-6));
}

TEST_CASE("control writes beyond the table are rejected") {
    engine e;
    CHECK_FALSE(e.setControl(-1, 1.0f));
    constexpr int beyond = 1'048'576;  // 2^20
    CHECK_FALSE(e.setControl(beyond, 1.0f));
    CHECK(e.control(beyond) == 0.0f);
}

TEST_CASE("a quantized swap on a block-aligned beat lands in the block that starts there") {
    // At 120 BPM and 48 kHz, beat k is sample 24000k: a block boundary for
    // blocks of 32 and 64 samples. The summed beat clock may end the
    // previous block a rounding error past the beat.
    for (const int block : {32, 64}) {
        INFO("block " << block);
        renderer r(1, 48000.0, block);
        r.play(sine(440.0, "s1"), 1);
        r.run(block);
        for (int k = 1; k <= 4; ++k) {
            INFO("beat " << k);
            r.play(sine(440.0, "s" + std::to_string(k + 1)), k + 1, 0.0, quantization::beat);
            r.run(24000 * k - static_cast<int>(r.out().size()));
            CHECK(r.engine.counters().activePlan == k);
            r.run(block);
            CHECK(r.engine.counters().activePlan == k + 1);
        }
        r.play(sine(440.0, "bar"), 6, 0.0, quantization::bar);  // bar 2 is beat 8: sample 192000
        r.run(192000 - static_cast<int>(r.out().size()));
        CHECK(r.engine.counters().activePlan == 5);
        r.run(block);
        CHECK(r.engine.counters().activePlan == 6);
    }
}

TEST_CASE("a non-finite or negative fade is no fade") {
    for (const double fade :
         {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::quiet_NaN(), -1.0}) {
        INFO("fade " << fade);
        renderer r(1);
        r.play(sine(440.0, "a"), 1, fade);
        r.run(64);
        r.play(sine(440.0, "b"), 2);  // waits for any fade still running
        r.run(128);
        CHECK(r.engine.counters().activePlan == 2);
        CHECK(has(r.engine.collect(), notice::kind::retired, 1));
    }
}

TEST_CASE("plans that share an ID swap and retire like any others") {
    renderer r(1);
    r.play(sine(440.0, "a"), 1);
    r.run(64);
    r.play(sine(440.0, "b"), 2, 0.0, quantization::bar);  // waits for its target
    r.play(sine(440.0, "c"), 2);                          // the same ID replaces it
    r.run(64);
    CHECK(r.engine.counters().activePlan == 2);
    r.play(sine(440.0, "d"), 3);  // built against the plan that is active
    r.run(128);
    CHECK(r.engine.counters().activePlan == 3);
    const auto ns = r.engine.collect();
    CHECK(std::count_if(ns.begin(), ns.end(),
                        [](const notice& n) { return n.what == notice::kind::retired && n.plan == 2; }) == 2);
    CHECK(has(ns, notice::kind::retired, 1));
}

TEST_CASE("an engine needs a positive block size and sample rate") {
    CHECK_THROWS_AS(engine({{48000.0, 0}, {}}), std::invalid_argument);
    CHECK_THROWS_AS(engine({{48000.0, -64}, {}}), std::invalid_argument);
    CHECK_THROWS_AS(engine({{0.0, 64}, {}}), std::invalid_argument);
    CHECK_THROWS_AS(engine({{std::numeric_limits<double>::quiet_NaN(), 64}, {}}), std::invalid_argument);
    CHECK_NOTHROW(engine({{48000.0, 1}, {}}));
}

TEST_CASE("an engine's counters report each thread's load, the audio thread first (threads.md 8)") {
    for (const int threads : {1, 3}) {
        INFO(threads << " threads");
        renderer r({.audio = {.sampleRate = 48000.0, .blockSize = 64}, .pipes = {}, .threads = threads}, 1);
        graph g;
        g.pipe(audio(0));
        for (int i = 0; i < 40; ++i) {
            auto& n = g.node("sine" + std::to_string(i), "sineOsc");
            n.params = {{"freq", 100.0 + i}};
            n.outputs["out"] = {audio(0)};
        }
        auto& out = g.node("out", "audioOut");
        out.inputs["in"] = {audio(0)};
        r.play(g.spec);
        r.run(64 * 200);
        const auto c = r.engine.counters();
        CHECK(c.threads == threads);
        REQUIRE(c.threadLoads.size() == static_cast<std::size_t>(threads));
        CHECK(c.threadLoads[0] > 0.0f);
        if (threads == 1) CHECK(c.threadLoads[0] == c.load);
    }
}

TEST_CASE("a plan whose tasks form a chain runs on the audio thread alone and counts its load") {
    renderer r({.audio = {.sampleRate = 48000.0, .blockSize = 64}, .pipes = {}, .threads = 4}, 1);
    graph g;
    g.pipe(audio(0)).pipe(audio(1));
    auto& s = g.node("sine", "sineOsc");
    s.params["freq"] = 440.0;
    s.outputs["out"] = {audio(0)};
    auto& half = g.node("half", "gain");
    half.params["amount"] = 0.5;
    half.inputs["in"] = {audio(0)};
    half.outputs["out"] = {audio(1)};
    g.node("out", "audioOut").inputs["in"] = {audio(1)};
    auto p = r.engine.build(g.spec, 1);
    CHECK_FALSE(p->schedule.parallel);
    r.engine.play(std::move(p), 0.0, quantization::none);
    r.run(64 * 200);
    const auto c = r.engine.counters();
    REQUIRE(c.threadLoads.size() == 4);
    CHECK(c.threadLoads[0] > 0.0f);
    for (std::size_t i = 1; i < c.threadLoads.size(); ++i) {
        INFO("thread " << i);
        CHECK(c.threadLoads[i] == 0.0f);  // no worker ran a task
    }
}

TEST_CASE("an engine refuses a thread count or worker spin or least task cost out of range") {
    const prepareInfo audio{.sampleRate = 48000.0, .blockSize = 64};
    CHECK_THROWS_AS(engine({.audio = audio, .pipes = {}, .minimumTaskCost = -1.0}), std::invalid_argument);
    CHECK_THROWS_AS(engine({.audio = audio, .pipes = {}, .minimumTaskCost = std::nan("")}),
                    std::invalid_argument);
    CHECK_NOTHROW(engine({.audio = audio, .pipes = {}, .minimumTaskCost = 0.0}));
    CHECK_THROWS_AS(engine({.audio = audio, .pipes = {}, .threads = 0}), std::invalid_argument);
    CHECK_THROWS_AS(engine({.audio = audio, .pipes = {}, .threads = maxThreads + 1}), std::invalid_argument);
    CHECK_THROWS_AS(
        engine({.audio = audio, .pipes = {}, .threads = 2, .workerSpin = std::chrono::microseconds(-1)}),
        std::invalid_argument);
    CHECK_NOTHROW(
        engine({.audio = audio, .pipes = {}, .threads = 2, .workerSpin = std::chrono::microseconds(0)}));
}
