// Offline renders of single generators through audioOut.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/graph.h"

using Catch::Approx;
using namespace support;

namespace {

constexpr int second = 48000;

// @p type writing audio pipe 0, played through device output 0.
graph source(const std::string& type, std::map<std::string, metadata::paramValue, std::less<>> params = {}) {
    graph g;
    g.pipe(audio(0));
    auto& n = g.node("gen", type);
    n.params = std::move(params);
    n.outputs["out"] = {audio(0)};
    g.node("out", "audioOut").inputs["in"] = {audio(0)};
    return g;
}

// A sine of @p freq into @p type, played through device output 0.
graph filtered(double freq, const std::string& type, std::map<std::string, metadata::paramValue, std::less<>> params) {
    graph g;
    g.pipe(audio(0)).pipe(audio(1));
    auto& s = g.node("sine", "sineOsc");
    s.params["freq"] = freq;
    s.outputs["out"] = {audio(0)};
    auto& f = g.node("filter", type);
    f.params = std::move(params);
    f.inputs["in"] = {audio(0)};
    f.outputs[type == "svf" ? "out" : "out"] = {audio(1)};
    g.node("out", "audioOut").inputs["in"] = {audio(1)};
    return g;
}

std::vector<float> render(const graph& g, int frames = second) {
    renderer r(1);
    r.play(g.spec);
    r.run(frames);
    return r.out(0);
}

} // namespace

TEST_CASE("oscillators run at their frequency within ±1") {
    for (const std::string type : {"sineOsc", "sawOsc", "pulseOsc", "triangleOsc"}) {
        INFO(type);
        const auto out = render(source(type, {{"freq", 1000.0}}));
        CHECK(std::abs(crossings(out) - 1000) <= 1);
        CHECK(peak(out) < 1.2);
        CHECK(peak(out) > 0.8);
    }
    CHECK(rms(render(source("sineOsc", {{"freq", 1000.0}}))) == Approx(std::sqrt(0.5)).epsilon(0.01));
}

TEST_CASE("an audio pipe on freq modulates the oscillator") {
    graph g;
    g.pipe(audio(0)).pipe(audio(1));
    auto& lfo = g.node("lfo", "lfo");
    lfo.params = {{"rate", 0.0}, {"depth", 0.0}, {"offset", 500.0}};
    lfo.outputs["out"] = {audio(0)};
    auto& s = g.node("sine", "sineOsc");
    s.params["freq"] = audio(0);
    s.outputs["out"] = {audio(1)};
    g.node("out", "audioOut").inputs["in"] = {audio(1)};
    CHECK(std::abs(crossings(render(g)) - 500) <= 1);
}

TEST_CASE("noise is deterministic per seed and node ID") {
    for (const std::string color : {"white", "pink", "brown"}) {
        INFO(color);
        auto g = source("noise", {{"color", color}});
        const auto a = render(g, 4096);
        CHECK(a == render(g, 4096));
        CHECK(rms(a) > 0.01);
        CHECK(peak(a) <= 1.5);
        g.spec.seed = 99;
        CHECK(a != render(g, 4096));
    }
}

TEST_CASE("biquad low-pass passes low and cuts high frequencies") {
    const auto low = render(filtered(100.0, "biquad", {{"freq", 1000.0}}));
    const auto high = render(filtered(10000.0, "biquad", {{"freq", 1000.0}}));
    CHECK(rms(low, second / 2) == Approx(std::sqrt(0.5)).epsilon(0.02));
    CHECK(rms(high, second / 2) < 0.02);
    const auto hp = render(filtered(100.0, "biquad", {{"freq", 1000.0}, {"mode", std::string("high-pass")}}));
    CHECK(rms(hp, second / 2) < 0.02);
}

TEST_CASE("biquad shelves and peaks apply their gain") {
    const auto boosted = render(filtered(1000.0, "biquad", {{"freq", 1000.0}, {"mode", std::string("peaking")}, {"gain", 2.0}}));
    CHECK(rms(boosted, second / 2) == Approx(2.0 * std::sqrt(0.5)).epsilon(0.02));
    const auto shelf = render(filtered(50.0, "biquad", {{"freq", 1000.0}, {"mode", std::string("low-shelf")}, {"gain", 0.5}}));
    CHECK(rms(shelf, second / 2) == Approx(0.5 * std::sqrt(0.5)).epsilon(0.05));
}

TEST_CASE("svf modes and simultaneous outputs") {
    CHECK(rms(render(filtered(10000.0, "svf", {{"cutoff", 500.0}})), second / 2) < 0.02);
    CHECK(rms(render(filtered(100.0, "svf", {{"cutoff", 5000.0}, {"mode", std::string("high")}})), second / 2) < 0.01);
    const auto band = render(filtered(1000.0, "svf", {{"cutoff", 1000.0}, {"mode", std::string("band")}}));
    CHECK(rms(band, second / 2) == Approx(std::sqrt(0.5) * 0.7071).epsilon(0.05));  // gain at fc is Q

    graph g;
    g.pipe(audio(0)).pipe(audio(1)).pipe(audio(2));
    auto& s = g.node("sine", "sineOsc");
    s.params["freq"] = 100.0;
    s.outputs["out"] = {audio(0)};
    auto& f = g.node("filter", "svf");
    f.params["cutoff"] = 1000.0;
    f.inputs["in"] = {audio(0)};
    f.outputs["low"] = {audio(1)};
    f.outputs["high"] = {audio(2)};
    g.node("out", "audioOut").inputs["in"] = {audio(1)};
    auto& o2 = g.node("out2", "audioOut");
    o2.params["channel"] = 1.0;
    o2.inputs["in"] = {audio(2)};
    renderer r(2);
    r.play(g.spec);
    r.run(second);
    CHECK(rms(r.out(0), second / 2) > 0.65);
    CHECK(rms(r.out(1), second / 2) < 0.02);
}

TEST_CASE("ladder filters and self-oscillates at resonance 1") {
    CHECK(rms(render(filtered(10000.0, "ladder", {{"cutoff", 500.0}})), second / 2) < 0.01);
    CHECK(rms(render(filtered(100.0, "ladder", {{"cutoff", 5000.0}})), second / 2) > 0.4);

    // A short burst excites the filter; it keeps ringing near the cutoff.
    graph g;
    g.pipe(audio(0)).pipe(audio(1));
    g.node("in", "audioIn").outputs["out"] = {audio(0)};
    auto& f = g.node("filter", "ladder");
    f.params = {{"cutoff", 1000.0}, {"resonance", 1.0}};
    f.inputs["in"] = {audio(0)};
    f.outputs["out"] = {audio(1)};
    g.node("out", "audioOut").inputs["in"] = {audio(1)};
    renderer r(1);
    r.play(g.spec);
    std::vector<std::vector<float>> impulse(1, std::vector<float>(second, 0.0f));
    impulse[0][0] = 1.0f;
    r.run(second, impulse);
    // It keeps ringing: the last quarter second is as loud as the one before.
    const double before = rms(r.out(0), second / 2, 3 * second / 4);
    const double after = rms(r.out(0), 3 * second / 4);
    CHECK(after > 0.02);
    CHECK(after > 0.9 * before);
    CHECK(std::abs(crossings(r.out(0), second / 2) - 500) < 40);  // half a second near 1 kHz
}

TEST_CASE("adsr without a gate ramps up and sustains") {
    graph g;
    g.pipe(audio(0));
    auto& e = g.node("env", "adsr");
    e.params = {{"attack", 0.01}, {"decay", 0.01}, {"sustain", 0.5}};
    e.outputs["out"] = {audio(0)};
    g.node("out", "audioOut").inputs["in"] = {audio(0)};
    const auto out = render(g, 4800);
    CHECK(out[240] == Approx(0.5).margin(0.01));   // halfway up the 10 ms attack
    CHECK(out[480] == Approx(1.0).margin(0.01));
    CHECK(out[4000] == Approx(0.5).margin(0.001));
}

TEST_CASE("adsr follows gate events and applies to its input") {
    graph g;
    g.pipe(events(0)).pipe(audio(0)).pipe(audio(1));
    auto& m = g.node("metro", "metro");
    m.params["every"] = 1.0;  // 120 BPM: a trigger every half second
    m.outputs["out"] = {events(0)};
    auto& s = g.node("sine", "sineOsc");
    s.outputs["out"] = {audio(0)};
    auto& e = g.node("env", "adsr");
    e.params = {{"attack", 0.001}, {"decay", 0.05}, {"sustain", 0.5}, {"release", 0.05}};
    e.inputs = {{"in", {audio(0)}}, {"gate", {events(0)}}};
    e.outputs["out"] = {audio(1)};
    g.node("out", "audioOut").inputs["in"] = {audio(1)};
    const auto out = render(g, second);
    // A trigger plays attack and decay, then releases: silence before the next beat.
    CHECK(peak(out, 0, 2400) > 0.9);
    CHECK(peak(out, 12000, 24000) < 1e-6);
    CHECK(peak(out, 24000, 26400) > 0.9);
}

TEST_CASE("delay line without interpolation delays by whole samples") {
    graph g;
    g.pipe(audio(0)).pipe(audio(1));
    g.node("in", "audioIn").outputs["out"] = {audio(0)};
    auto& d = g.node("delay", "delayLine");
    d.params = {{"time", 100.0 / second}, {"max-time", 0.01}, {"interpolation", std::string("none")}};
    d.inputs["in"] = {audio(0)};
    d.outputs["out"] = {audio(1)};
    g.node("out", "audioOut").inputs["in"] = {audio(1)};
    renderer r(1);
    r.play(g.spec);
    std::vector<std::vector<float>> impulse(1, std::vector<float>(1024, 0.0f));
    impulse[0][10] = 1.0f;
    r.run(1024, impulse);
    CHECK(r.out(0)[110] == Approx(1.0f));
    CHECK(peak(r.out(0), 0, 110) == 0.0);
    CHECK(peak(r.out(0), 111) == 0.0);
}

TEST_CASE("comb and allpass ring out and stay stable") {
    for (const std::string type : {"comb", "allpass"}) {
        INFO(type);
        graph g;
        g.pipe(audio(0)).pipe(audio(1));
        g.node("in", "audioIn").outputs["out"] = {audio(0)};
        auto& d = g.node("delay", type);
        d.params = {{"time", 0.001}, {"feedback", 0.9}};
        d.inputs["in"] = {audio(0)};
        d.outputs["out"] = {audio(1)};
        g.node("out", "audioOut").inputs["in"] = {audio(1)};
        renderer r(1);
        r.play(g.spec);
        std::vector<std::vector<float>> impulse(1, std::vector<float>(second, 0.0f));
        impulse[0][0] = 1.0f;
        r.run(second, impulse);
        CHECK(peak(r.out(0), 48, 200) > 0.1);         // echoes after 1 ms
        CHECK(peak(r.out(0)) <= 1.0 + 1e-6);
        CHECK(peak(r.out(0), second - 4800) < 1e-3);  // decayed after a second
    }
}

TEST_CASE("pan is equal power") {
    graph g;
    g.pipe(audio(0)).pipe(audio(1), 2);
    auto& s = g.node("sine", "sineOsc");
    s.outputs["out"] = {audio(0)};
    auto& p = g.node("pan", "pan");
    p.params["position"] = -0.5;
    p.inputs["in"] = {audio(0)};
    p.outputs["out"] = {audio(1)};
    g.node("out", "audioOut").inputs["in"] = {audio(1)};
    renderer r(2);
    r.play(g.spec);
    r.run(second);
    const double l = rms(r.out(0)), right = rms(r.out(1));
    CHECK(l * l + right * right == Approx(0.5).epsilon(0.01));
    CHECK(l > right);
}
