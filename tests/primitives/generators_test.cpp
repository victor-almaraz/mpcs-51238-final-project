// Offline renders of single generators through audioOut.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/io.h>

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
    f.outputs["out"] = {audio(1)};
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
    // A cut, not a boost: audioOut limits a full-scale sine boosted past 1.
    const auto cut = render(filtered(1000.0, "biquad", {{"freq", 1000.0}, {"mode", std::string("peaking")}, {"gain", 0.5}}));
    CHECK(rms(cut, second / 2) == Approx(0.5 * std::sqrt(0.5)).epsilon(0.02));
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

TEST_CASE("svf writes each output with its own channel count") {
    // The first output (low) has one channel and notch has two: the second
    // notch channel carries the filtered mono input too.
    graph g;
    g.pipe(audio(0)).pipe(audio(1)).pipe(audio(2), 2);
    auto& s = g.node("sine", "sineOsc");
    s.params["freq"] = 100.0;
    s.outputs["out"] = {audio(0)};
    auto& f = g.node("filter", "svf");
    f.params["cutoff"] = 5000.0;
    f.inputs["in"] = {audio(0)};
    f.outputs["low"] = {audio(1)};
    f.outputs["notch"] = {audio(2)};
    g.node("out", "audioOut").inputs["in"] = {audio(2)};
    renderer r(2);
    r.play(g.spec);
    r.run(second / 4);
    CHECK(rms(r.out(0), second / 8) > 0.6);
    CHECK(rms(r.out(1), second / 8) == Approx(rms(r.out(0), second / 8)).epsilon(1e-6));
}

TEST_CASE("lfo with a negative rate keeps cycling") {
    graph g;
    g.pipe(control(1)).pipe(control(2));
    auto& l = g.node("lfo", "lfo");
    l.params = {{"shape", std::string("sample-and-hold")}, {"rate", control(1)}};
    l.outputs["out"] = {control(2)};
    renderer r(1);
    r.play(g.spec);
    r.engine.setControl(1, -100.0f);
    r.run(64);
    // 100 blocks of 64 samples at 100 Hz: 6400 / 480 ≈ 13 new held values.
    int changes = 0;
    float previous = r.engine.control(2);
    for (int k = 0; k < 100; ++k) {
        r.run(64);
        const float v = r.engine.control(2);
        changes += v != previous ? 1 : 0;
        previous = v;
    }
    CHECK(changes >= 10);
}

TEST_CASE("a delay whose max-time is below one sample delays by one sample") {
    graph g;
    g.pipe(audio(0)).pipe(audio(1));
    g.node("in", "audioIn").outputs["out"] = {audio(0)};
    auto& d = g.node("delay", "comb");
    d.params = {{"time", 0.01}, {"max-time", 0.0}, {"feedback", 0.0}};
    d.inputs["in"] = {audio(0)};
    d.outputs["out"] = {audio(1)};
    g.node("out", "audioOut").inputs["in"] = {audio(1)};
    renderer r(1);
    r.play(g.spec);
    std::vector<std::vector<float>> impulse(1, std::vector<float>(64, 0.0f));
    impulse[0][0] = 1.0f;
    r.run(64, impulse);
    // The comb's shortest delay is one sample: the impulse at 0 is at 1.
    CHECK(r.out(0)[1] == Approx(1.0f));
    CHECK(peak(r.out(0), 2) == 0.0);
}

TEST_CASE("biquad reads its frequency per channel") {
    // Device inputs set the cutoff: 100 Hz on channel 0, 10 kHz on channel 1.
    graph g;
    g.pipe(audio(0), 2).pipe(audio(1)).pipe(audio(2), 2);
    g.node("cutoffs", "audioIn").outputs["out"] = {audio(0)};
    auto& s = g.node("sine", "sineOsc");
    s.params["freq"] = 1000.0;
    s.outputs["out"] = {audio(1)};
    auto& f = g.node("filter", "biquad");
    f.params["freq"] = audio(0);
    f.inputs["in"] = {audio(1)};
    f.outputs["out"] = {audio(2)};
    g.node("out", "audioOut").inputs["in"] = {audio(2)};
    renderer r(2);
    r.play(g.spec);
    const std::vector<std::vector<float>> cutoffs{std::vector<float>(second / 4, 100.0f),
                                                  std::vector<float>(second / 4, 10000.0f)};
    r.run(second / 4, cutoffs);
    CHECK(rms(r.out(0), second / 8) < 0.02);
    CHECK(rms(r.out(1), second / 8) > 0.6);
}

TEST_CASE("biquad follows a frequency that changes within a block") {
    // One 4096-sample block: the cutoff steps from 10 kHz to 50 Hz at
    // sample 1024, so the end of the block is filtered.
    graph g;
    g.pipe(audio(0)).pipe(audio(1)).pipe(audio(2));
    g.node("cutoff", "audioIn").outputs["out"] = {audio(0)};
    auto& s = g.node("sine", "sineOsc");
    s.params["freq"] = 2000.0;
    s.outputs["out"] = {audio(1)};
    auto& f = g.node("filter", "biquad");
    f.params["freq"] = audio(0);
    f.inputs["in"] = {audio(1)};
    f.outputs["out"] = {audio(2)};
    g.node("out", "audioOut").inputs["in"] = {audio(2)};
    renderer r(1, 48000.0, 4096);
    r.play(g.spec);
    std::vector<std::vector<float>> cutoff(1, std::vector<float>(4096, 50.0f));
    std::fill(cutoff[0].begin(), cutoff[0].begin() + 1024, 10000.0f);
    r.run(4096, cutoff);
    CHECK(rms(r.out(0), 256, 1024) > 0.6);
    CHECK(rms(r.out(0), 3072) < 0.02);
}

TEST_CASE("audio and control reads map missing channels the same way") {
    // A two-channel input read as three channels: channel 2 of the audio
    // output and channel 2 of the control output hold the same value.
    graph g;
    g.pipe(audio(0), 2).pipe(audio(1), 3).pipe(control(1), 3);
    g.node("in", "audioIn").outputs["out"] = {audio(0)};
    auto& a = g.node("audio", "gain");
    a.inputs["in"] = {audio(0)};
    a.outputs["out"] = {audio(1)};
    auto& c = g.node("control", "gain");
    c.inputs["in"] = {audio(0)};
    c.outputs["out"] = {control(1)};
    g.node("out", "audioOut").inputs["in"] = {audio(1)};
    renderer r(3);
    r.play(g.spec);
    r.run(64, {std::vector<float>(64, 0.25f), std::vector<float>(64, 0.5f)});
    const float control2 = r.engine.pipes().control(1).value(2);
    CHECK(r.out(2).back() == control2);
    CHECK(control2 == 0.25f);  // channels wrap: channel 2 of two reads channel 0
}

TEST_CASE("a pipe declared with too many channels is clamped") {
    graph g;
    g.pipe(audio(0), 20);
    g.node("sine", "sineOsc").outputs["out"] = {audio(0)};
    g.node("out", "audioOut").inputs["in"] = {audio(0)};
    renderer r(metadata::maxChannels);
    r.play(g.spec);
    r.run(4800);
    CHECK(r.engine.pipes().audio(0).channels() == metadata::maxChannels);
    for (int ch = 0; ch < metadata::maxChannels; ++ch) CHECK(rms(r.out(ch)) > 0.6);
}

TEST_CASE("adsr holds while any note is down") {
    graph g;
    g.pipe(events(0)).pipe(audio(0));
    g.node("midi", "midiIn").outputs["out"] = {events(0)};
    auto& e = g.node("env", "adsr");
    e.params = {{"attack", 0.001}, {"decay", 0.001}, {"sustain", 0.5}, {"release", 0.01}};
    e.inputs["gate"] = {events(0)};
    e.outputs["out"] = {audio(0)};
    g.node("out", "audioOut").inputs["in"] = {audio(0)};
    renderer r(1);
    r.play(g.spec);
    r.run(64);
    auto note = [&](std::uint8_t status, std::uint8_t number) {
        r.engine.io().deliverMidi(
            "k", primitives::midiPacket{.time = 0.0, .size = 3, .bytes = {status, number, 100}});
    };
    note(0x90, 60);
    note(0x90, 64);
    r.run(4800);
    note(0x80, 60);  // 64 is still held
    r.run(4800);
    CHECK(r.out().back() == Approx(0.5f));
    note(0x80, 64);
    r.run(4800);
    CHECK(r.out().back() == 0.0f);
}

TEST_CASE("element-wise operations compute their functions on both paths") {
    struct opCase {
        std::string type;
        std::map<std::string, metadata::paramValue, std::less<>> params;
        float in;
        float expected;
    };
    const std::vector<opCase> cases{
        {.type = "gain", .params = {{"amount", 3.0}}, .in = 2.0f, .expected = 6.0f},
        {.type = "clip", .params = {{"low", -1.0}, {"high", 1.0}}, .in = 2.0f, .expected = 1.0f},
        {.type = "clamp", .params = {{"low", 1.0}, {"high", -1.0}}, .in = -2.0f, .expected = -1.0f},
        {.type = "dbToAmp", .params = {}, .in = 20.0f, .expected = 10.0f},
        {.type = "beatsToSeconds", .params = {}, .in = 2.0f, .expected = 1.0f},  // 120 BPM
        {.type = "beatsToSeconds", .params = {{"beats", 1.0}}, .in = 2.0f, .expected = 1.5f},
        {.type = "neg", .params = {}, .in = 2.0f, .expected = -2.0f},
        {.type = "abs", .params = {}, .in = -2.0f, .expected = 2.0f},
        {.type = "exp", .params = {}, .in = 0.0f, .expected = 1.0f},
        {.type = "log", .params = {}, .in = 1.0f, .expected = 0.0f},
        {.type = "sin", .params = {}, .in = 0.0f, .expected = 0.0f},
        {.type = "cos", .params = {}, .in = 0.0f, .expected = 1.0f},
        {.type = "tanh", .params = {}, .in = 0.0f, .expected = 0.0f},
        {.type = "floor", .params = {}, .in = 1.5f, .expected = 1.0f},
        {.type = "round", .params = {}, .in = 1.5f, .expected = 2.0f},
        {.type = "subtract", .params = {{"b", 0.5}}, .in = 2.0f, .expected = 1.5f},
        {.type = "divide", .params = {{"b", 4.0}}, .in = 2.0f, .expected = 0.5f},
        {.type = "divide", .params = {{"b", 0.0}}, .in = 2.0f, .expected = 0.0f},
        {.type = "pow", .params = {{"b", 3.0}}, .in = 2.0f, .expected = 8.0f},
        {.type = "min", .params = {{"b", 1.0}}, .in = 2.0f, .expected = 1.0f},
        {.type = "max", .params = {{"b", 3.0}}, .in = 2.0f, .expected = 3.0f},
        {.type = "quantize", .params = {{"step", 0.5}}, .in = 1.3f, .expected = 1.5f},
        {.type = "scale",
         .params = {{"in-low", 0.0}, {"in-high", 4.0}, {"out-low", 10.0}, {"out-high", 20.0}},
         .in = 2.0f,
         .expected = 15.0f},
        {.type = "scaleExp",
         .params = {{"in-low", 0.0}, {"in-high", 1.0}, {"out-low", 1.0}, {"out-high", 100.0}},
         .in = 0.5f,
         .expected = 10.0f},
    };
    for (const auto& c : cases) {
        INFO(c.type);
        for (const bool audioPath : {false, true}) {
            INFO(audioPath);
            graph g;
            g.pipe(control(1)).pipe(control(2)).pipe(audio(0));
            auto& n = g.node("op", c.type);
            n.params = c.params;
            n.inputs["in"] = {control(1)};
            n.outputs["out"] = {audioPath ? audio(0) : control(2)};
            renderer r(1);
            r.play(g.spec);
            r.engine.setControl(1, c.in);
            r.run(64);
            const float out = audioPath ? r.engine.pipes().audio(0).read(0).back() : r.engine.control(2);
            CHECK(out == Approx(c.expected).margin(1e-5));
        }
    }
}
