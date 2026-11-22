// Audio hazards at run time: the output limiter, muting on non-finite
// samples, and rejected non-finite inputs.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/event.h>
#include <espresso/primitives/io.h>
#include <espresso/primitives/plan.h>
#include <espresso/primitives/primitive.h>

#include "support/graph.h"

using namespace support;
using namespace primitives;

namespace {

// A sine at @p amplitude through audioOut with @p ceiling.
metadata::graphSpec loud(double amplitude, double ceiling = 1.0, const std::string& id = "out") {
    graph g;
    g.pipe(audio(0)).pipe(audio(1));
    g.node("sine", "sineOsc").outputs["out"] = {audio(0)};
    auto& gain = g.node("gain", "gain");
    gain.params["amount"] = amplitude;
    gain.inputs["in"] = {audio(0)};
    gain.outputs["out"] = {audio(1)};
    auto& out = g.node(id, "audioOut");
    out.params["ceiling"] = ceiling;
    out.inputs["in"] = {audio(1)};
    return g.spec;
}

// Noise into a loop through a feedback pipe with gain 1.5: grows to infinity.
metadata::graphSpec runaway() {
    graph g;
    g.pipe(audio(0), 1, true).pipe(audio(1)).pipe(audio(2));
    g.node("noise", "noise").outputs["out"] = {audio(1)};
    auto& add = g.node("add", "add");
    add.inputs["in"] = {audio(1), audio(0)};
    add.outputs["out"] = {audio(2)};
    auto& gain = g.node("gain", "gain");
    gain.params["amount"] = 1.5;
    gain.inputs["in"] = {audio(2)};
    gain.outputs["out"] = {audio(0)};
    g.node("out", "audioOut").inputs["in"] = {audio(2)};
    return g.spec;
}

} // namespace

TEST_CASE("signals within the ceiling pass unchanged") {
    renderer limited(1), reference(1);
    limited.play(loud(0.5));
    limited.run(4800);
    // The same sine without the gain stage's limiter headroom question:
    // a sine at 0.5 through audioOut is exactly the sine scaled by 0.5.
    graph g;
    g.pipe(audio(0));
    g.node("sine", "sineOsc").outputs["out"] = {audio(0)};
    g.node("out", "audioOut").inputs["in"] = {audio(0)};
    reference.play(g.spec);
    reference.run(4800);
    for (std::size_t i = 0; i < 4800; ++i) REQUIRE(limited.out()[i] == reference.out()[i] * 0.5f);
}

TEST_CASE("the limiter holds a loud signal at the ceiling") {
    renderer r(1);
    r.play(loud(1000.0));
    r.run(48000);
    CHECK(peak(r.out()) <= 1.0);
    CHECK(rms(r.out(), 24000) > 0.5);  // limited, not muted

    renderer quiet(1);
    quiet.play(loud(4.0, 0.501187));  // a -6 dB ceiling
    quiet.run(48000);
    CHECK(peak(quiet.out()) <= 0.501187 + 1e-6);
}

TEST_CASE("non-finite samples mute the output and report a fault") {
    renderer r(1);
    r.play(runaway(), 1);
    r.run(48000 * 2);
    CHECK(std::all_of(r.out().begin(), r.out().end(), [](float x) { return std::isfinite(x); }));
    CHECK(peak(r.out()) <= 1.0);
    CHECK(peak(r.out(), r.out().size() - 4800) == 0.0);  // muted
    CHECK(r.engine.counters().faults == 1);
    const auto faults = r.engine.io().takeFaults();
    REQUIRE(faults.size() == 1);
    CHECK_THAT(faults[0], Catch::Matchers::StartsWith("out: non-finite audio samples"));

    // The next plan plays again.
    r.play(loud(0.5), 2);
    r.run(4800);
    CHECK(rms(r.out(), r.out().size() - 2400) > 0.3);
}

TEST_CASE("the node that reports faults is the node that declares it (threads.md 4.1)") {
    // audioOut takes the reporter it reports through from the declaration,
    // so the plan orders it as a reporter; no other node reports.
    renderer r(1);
    const auto p = r.engine.build(runaway(), 1);
    REQUIRE(p->info.size() == p->nodes.size());
    for (std::size_t i = 0; i < p->nodes.size(); ++i) {
        INFO(p->nodes[i]->id());
        CHECK(p->info[i].reportsFaults == (p->nodes[i]->id() == "out"));
    }
}

TEST_CASE("a node adopts no non-finite state, so a fixed patch plays again") {
    // Plan 1 drives one parameter (or the input) of the node under test
    // with pow(-1, 0.5) = NaN, which poisons its state and mutes the output.
    // Plan 2 keeps the node's ID and shape but reads a finite control value:
    // the adopted node must start clean instead of carrying the NaN over.
    struct nodeCase {
        std::string type;
        std::string param;  // empty: the control pipe is the node's input
        float good;
        bool audioInput;
        std::map<std::string, metadata::paramValue, std::less<>> fixed;
    };
    const std::vector<nodeCase> cases{
        {.type = "biquad", .param = "freq", .good = 1000.0f, .audioInput = true, .fixed = {}},
        {.type = "svf", .param = "cutoff", .good = 1000.0f, .audioInput = true, .fixed = {}},
        {.type = "ladder", .param = "cutoff", .good = 1000.0f, .audioInput = true, .fixed = {}},
        {.type = "comb", .param = "feedback", .good = 0.5f, .audioInput = true, .fixed = {}},
        {.type = "allpass", .param = "feedback", .good = 0.5f, .audioInput = true, .fixed = {}},
        {.type = "delayLine", .param = "", .good = 0.5f, .audioInput = false, .fixed = {{"time", 0.001}}},
        {.type = "sineOsc", .param = "freq", .good = 440.0f, .audioInput = false, .fixed = {}},
        {.type = "triangleOsc", .param = "freq", .good = 440.0f, .audioInput = false, .fixed = {}},
        {.type = "lfo", .param = "rate", .good = 5.0f, .audioInput = false, .fixed = {}},
        {.type = "adsr", .param = "attack", .good = 0.001f, .audioInput = false, .fixed = {}},
        {.type = "smoother", .param = "", .good = 0.5f, .audioInput = false, .fixed = {}},
    };
    for (const auto& c : cases) {
        INFO(c.type);
        auto make = [&](bool bad) {
            graph g;
            g.pipe(audio(0)).pipe(audio(1)).pipe(control(1)).pipe(control(2)).pipe(control(3));
            auto& s = g.node("sine", "sineOsc");
            s.params["freq"] = 220.0;
            s.outputs["out"] = {audio(0)};
            if (bad) {
                auto& p = g.node("nan", "pow");
                p.params["b"] = 0.5;
                p.inputs["in"] = {control(1)};
                p.outputs["out"] = {control(2)};
            }
            auto& n = g.node("node", c.type);
            n.params = c.fixed;
            const pipeRef value = bad ? control(2) : control(3);
            if (c.param.empty()) {
                n.inputs["in"] = {value};
            } else {
                n.params[c.param] = value;
            }
            if (c.audioInput) n.inputs["in"] = {audio(0)};
            n.outputs["out"] = {audio(1)};
            g.node("out", "audioOut").inputs["in"] = {audio(1)};
            return g.spec;
        };
        renderer r(1);
        r.engine.setControl(1, -1.0f);
        r.engine.setControl(3, c.good);
        r.play(make(true), 1);
        r.run(4800);
        CHECK(r.engine.io().takeFaults().size() == 1);
        r.play(make(false), 2);
        r.run(4800 * 2);
        CHECK(r.engine.io().takeFaults().empty());
        CHECK(peak(r.out(), r.out().size() - 4800) > 0.01);
    }
}

TEST_CASE("a non-finite delay time falls back to the shortest delay") {
    graph g;
    g.pipe(audio(0)).pipe(audio(1)).pipe(control(1)).pipe(control(2));
    g.node("in", "audioIn").outputs["out"] = {audio(0)};
    auto& p = g.node("nan", "pow");
    p.params["b"] = 0.5;
    p.inputs["in"] = {control(1)};
    p.outputs["out"] = {control(2)};
    auto& d = g.node("delay", "delayLine");
    d.params = {{"time", control(2)}, {"interpolation", std::string("none")}};
    d.inputs["in"] = {audio(0)};
    d.outputs["out"] = {audio(1)};
    g.node("out", "audioOut").inputs["in"] = {audio(1)};
    renderer r(1);
    r.engine.setControl(1, -1.0f);
    r.play(g.spec);
    r.run(64, {std::vector<float>(64, 0.5f)});
    CHECK(r.engine.io().takeFaults().empty());
    CHECK(r.out().back() == 0.5f);  // no interpolation: the shortest delay is 0 samples
}

TEST_CASE("oscOut rejects an address that does not start with a slash") {
    graph g;
    g.pipe(control(1));
    auto& out = g.node("send", "oscOut");
    out.params = {{"port", 9300.0}, {"address", std::string("cutoff")}};
    out.inputs["value"] = {control(1)};
    engine e;
    CHECK_THROWS_AS(e.build(g.spec, 1), buildError);
}

TEST_CASE("the builder rejects unvalidated nodes instead of building them") {
    // Graphs normally pass ir::validate first; the builder still refuses
    // what would leave a null pipe or an out-of-range index at run time.
    engine e;
    {
        graph g;  // gain without its required input
        g.pipe(audio(0));
        g.node("g", "gain").outputs["out"] = {audio(0)};
        CHECK_THROWS_AS(e.build(g.spec, 1), buildError);
    }
    {
        graph g;  // voiceSplit with an empty output list
        g.pipe(events(0));
        auto& split = g.node("split", "voiceSplit");
        split.inputs["in"] = {events(0)};
        split.outputs["out"] = {};
        CHECK_THROWS_AS(e.build(g.spec, 2), buildError);
    }
    {
        graph g;  // a delay whose max-time is not a number
        g.pipe(audio(0));
        g.pipe(audio(1));
        auto& d = g.node("d", "delayLine");
        d.inputs["in"] = {audio(0)};
        d.outputs["out"] = {audio(1)};
        d.params = {{"max-time", std::nan("")}};
        CHECK_THROWS_AS(e.build(g.spec, 3), buildError);
    }
}

TEST_CASE("non-finite control values are rejected") {
    engine e;
    CHECK_FALSE(e.setControl(1, std::numeric_limits<float>::quiet_NaN()));
    CHECK_FALSE(e.setControl(1, std::numeric_limits<float>::infinity()));
    CHECK(e.setControl(1, 0.5f));

    graph g;
    g.pipe(control(1));
    auto& in = g.node("osc", "oscIn");
    in.params["port"] = 9200.0;
    in.outputs["value"] = {control(1)};
    renderer r;
    r.play(g.spec);
    r.run(64);
    oscPacket p;
    p.size = static_cast<std::uint16_t>(encodeOscFloat(p.bytes, "/x", 0.25f));
    r.engine.io().deliverOsc(9200, p);
    r.run(64);
    p.size =
        static_cast<std::uint16_t>(encodeOscFloat(p.bytes, "/x", std::numeric_limits<float>::quiet_NaN()));
    r.engine.io().deliverOsc(9200, p);
    r.run(64);
    CHECK(r.engine.control(1) == 0.25f);
    CHECK(r.engine.counters().drops == 1);
}
