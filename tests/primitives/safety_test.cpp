// Audio hazards at run time: the output limiter, muting on non-finite
// samples, and rejected non-finite inputs.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "support/graph.h"

using Catch::Approx;
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

TEST_CASE("non-finite control values are rejected") {
    engine e;
    CHECK_FALSE(e.setControl(1, NAN));
    CHECK_FALSE(e.setControl(1, INFINITY));
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
    p.size = static_cast<std::uint16_t>(encodeOscFloat(p.bytes, "/x", NAN));
    r.engine.io().deliverOsc(9200, p);
    r.run(64);
    CHECK(r.engine.control(1) == 0.25f);
    CHECK(r.engine.counters().drops == 1);
}
