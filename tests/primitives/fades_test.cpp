// Second-review regressions: where fades apply, rewired nodes, held notes
// across swaps, quantization after a fade, and shared device channels.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/graph.h"

using Catch::Approx;
using namespace support;
using namespace primitives;

namespace {

metadata::graphSpec empty() { return {}; }

// A constant of @p level on an audio pipe.
void constant(graph& g, const std::string& id, int pipeIndex, double level) {
    auto& n = g.node(id, "lfo");
    n.params = {{"depth", 0.0}, {"offset", level}};
    n.outputs["out"] = {audio(pipeIndex)};
}

} // namespace

TEST_CASE("a carrier modulated by a new modulator fades in with it") {
    graph g;
    g.pipe(audio(0)).pipe(audio(1));
    constant(g, "mod", 0, 440.0);
    auto& s = g.node("carrier", "sineOsc");
    s.params["freq"] = audio(0);
    s.outputs["out"] = {audio(1)};
    g.node("out", "audioOut").inputs["in"] = {audio(1)};
    renderer r(1);
    r.play(g.spec, 1, 0.1);  // 4800 samples
    r.run(4800);
    CHECK(peak(r.out(), 0, 480) < 0.15);
    CHECK(peak(r.out(), 4320, 4800) > 0.85);
}

TEST_CASE("signal circulating in a feedback loop fades out") {
    graph g;
    g.pipe(audio(0), 1, true).pipe(audio(1)).pipe(audio(2)).pipe(audio(3));
    g.node("in", "audioIn").outputs["out"] = {audio(1)};
    auto& add = g.node("add", "add");
    add.inputs["in"] = {audio(1), audio(0)};
    add.outputs["out"] = {audio(2)};
    auto& d = g.node("delay", "delayLine");
    d.params["time"] = 0.002;
    d.inputs["in"] = {audio(2)};
    d.outputs["out"] = {audio(3)};
    auto& fb = g.node("fb", "gain");
    fb.params["amount"] = 0.95;
    fb.inputs["in"] = {audio(3)};
    fb.outputs["out"] = {audio(0)};
    g.node("out", "audioOut").inputs["in"] = {audio(3)};
    renderer r(1, 48000.0, 64);
    r.play(g.spec);
    std::vector<std::vector<float>> impulse(1, std::vector<float>(4800, 0.0f));
    impulse[0][0] = 1.0f;
    r.run(4800, impulse);
    const double before = peak(r.out(), 3840, 4800);
    REQUIRE(before > 0.2);
    r.play(empty(), 2, 0.05);  // 2400 samples
    r.run(4800);
    CHECK(peak(r.out(), 4800 + 2400 - 240, 4800 + 2400) < 0.2 * before);  // faded, not cut
    CHECK(peak(r.out(), 4800 + 2560) == 0.0);
}

TEST_CASE("a node whose input is rewired crossfades instead of switching") {
    auto plan = [](bool withBoost) {
        graph g;
        g.pipe(audio(0)).pipe(audio(1)).pipe(audio(2));
        constant(g, "source", 0, 0.5);
        int last = 0;
        if (withBoost) {
            auto& b = g.node("boost", "gain");
            b.params["amount"] = 2.0;
            b.inputs["in"] = {audio(0)};
            b.outputs["out"] = {audio(1)};
            last = 1;
        }
        g.node("out", "audioOut").inputs["in"] = {audio(last)};
        return g.spec;
    };
    renderer r(1);
    r.play(plan(true), 1);
    r.run(640);
    CHECK(r.out().back() == Approx(1.0f));
    r.play(plan(false), 2, 0.1);  // 4800 samples, 1.0 to 0.5
    r.run(4800);
    CHECK(r.out()[640 + 2400] == Approx(0.75f).margin(0.03f));
    CHECK(r.out().back() == Approx(0.5f).margin(0.01f));
}

TEST_CASE("a note held across a swap that replaces its voice split ends") {
    auto plan = [](int voices) {
        graph g;
        g.pipe(events(0)).pipe(control(1));
        g.node("midi", "midiIn").outputs["out"] = {events(0)};
        auto& v = g.node("voices", "voiceSplit");
        v.inputs["in"] = {events(0)};
        for (int i = 0; i < voices; ++i) {
            g.pipe(events(1 + i));
            v.outputs["out"].push_back(events(1 + i));
        }
        auto& gate = g.node("gate", "gate");
        gate.inputs["in"] = {events(1)};
        gate.outputs["out"] = {control(1)};
        return g.spec;
    };
    renderer r;
    r.play(plan(2), 1);
    r.run(64);
    midiPacket on;
    on.size = 3;
    on.bytes = {0x90, 60, 100};
    r.engine.io().deliverMidi("k", on);
    r.run(64);
    REQUIRE(r.engine.control(1) == 1.0f);
    r.play(plan(3), 2, 0.01);  // a new shape: the split is replaced, the gate adopted
    r.run(1024);
    CHECK(r.engine.control(1) == 0.0f);
}

TEST_CASE("a quantized swap that waited for a fade stays on the beat grid") {
    graph a, b, c;
    for (graph& g : {std::ref(a), std::ref(b), std::ref(c)}) g.pipe(audio(0));
    constant(a, "a", 0, 0.1);
    constant(b, "b", 0, 0.1);
    constant(c, "c", 0, 0.1);
    renderer r(1, 48000.0, 100);
    r.play(a.spec, 1);
    r.run(100);
    r.play(b.spec, 2, 0.6);  // fades until sample 28900
    r.run(900);
    r.play(c.spec, 3, 0.0, quantization::beat);  // beat 1 (24000) passes during the fade
    r.run(40000 - 1000);
    CHECK(r.engine.counters().activePlan == 2);  // waits for beat 2, sample 48000
    r.run(8100);
    CHECK(r.engine.counters().activePlan == 3);
}

TEST_CASE("sinks sharing a device channel never exceed full scale") {
    graph g;
    g.pipe(audio(0));
    constant(g, "level", 0, 0.9);
    g.node("one", "audioOut").inputs["in"] = {audio(0)};
    g.node("two", "audioOut").inputs["in"] = {audio(0)};
    renderer r(1);
    r.play(g.spec);
    r.run(640);
    CHECK(peak(r.out()) == 1.0);
}
