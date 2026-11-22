// fft and ifft: the round trip of samples.md §10.2 at every size and
// overlap, frames at hops shorter and longer than a block, one frame pipe
// read by two nodes, and frames a reader cannot use.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>

#include "support/bits.h"
#include "support/graph.h"

using Catch::Approx;
using support::audio;
using support::events;

namespace {

// White noise from a fixed linear congruential generator, at 0.5.
std::vector<float> noise(std::size_t n) {
    std::vector<float> x(n);
    std::uint32_t s = 12345;
    for (auto& v : x) {
        s = (s * 1664525u) + 1013904223u;
        v = (static_cast<float>(s >> 8u) / 16777216.0f - 0.5f);
    }
    return x;
}

// Device input -> fft -> ifft -> output 0, and a second ifft of the same
// frames to output 1.
support::graph roundTrip(double size, double overlap) {
    support::graph g;
    g.pipe(audio(0)).pipe(events(1)).pipe(audio(2)).pipe(audio(3));
    g.node("in", "audioIn").outputs["out"] = {audio(0)};
    auto& f = g.node("fft", "fft");
    f.params["size"] = size;
    f.params["overlap"] = overlap;
    f.inputs["in"] = {audio(0)};
    f.outputs["out"] = {events(1)};
    for (const auto& [id, pipe] : {std::pair{"ifft", audio(2)}, std::pair{"ifft2", audio(3)}}) {
        auto& i = g.node(id, "ifft");
        i.inputs["in"] = {events(1)};
        i.outputs["out"] = {pipe};
    }
    g.node("out", "audioOut").inputs["in"] = {audio(2)};
    auto& second = g.node("out2", "audioOut");
    second.params["channel"] = 1.0;
    second.inputs["in"] = {audio(3)};
    return g;
}

}  // namespace

TEST_CASE("fft then ifft reproduces the input exactly size samples later (samples.md 10.2)") {
    for (const double size : {256.0, 512.0, 1024.0, 2048.0, 4096.0, 8192.0, 16384.0}) {
        for (const double overlap : {2.0, 4.0}) {
            // Blocks shorter and longer than the hop: one frame in some
            // blocks and none in others, or several in each.
            for (const int block : {64, 1024}) {
                INFO("size " << size << ", overlap " << overlap << ", block " << block);
                const auto n = static_cast<std::size_t>(size);
                support::renderer r(2, 48000.0, block);
                r.play(roundTrip(size, overlap).spec);
                const auto x = noise(3 * n);
                r.run(static_cast<int>(3 * n), {x});
                double worst = 0.0;
                double worstSecond = 0.0;
                for (std::size_t t = 0; t < 3 * n; ++t) {
                    const double expected = t < n ? 0.0 : x[t - n];
                    worst = std::max(worst, std::abs(r.out(0)[t] - expected));
                    worstSecond = std::max(worstSecond, std::abs(r.out(1)[t] - expected));
                }
                CHECK(worst < 1e-5);
                CHECK(worstSecond < 1e-5);  // a second reader of the same frames
            }
        }
    }
}

TEST_CASE("an fft analyzes every channel of its input") {
    support::graph g;
    g.pipe(audio(0)).pipe(audio(1), 2).pipe(events(2)).pipe(audio(3), 2);
    g.node("in", "audioIn").outputs["out"] = {audio(0)};
    // Panned hard left: channel 0 is the input, channel 1 silent.
    auto& left = g.node("pan", "pan");
    left.params["position"] = -1.0;
    left.inputs["in"] = {audio(0)};
    left.outputs["out"] = {audio(1)};
    auto& f = g.node("fft", "fft");
    f.params["size"] = 256.0;
    f.inputs["in"] = {audio(1)};
    f.outputs["out"] = {events(2)};
    auto& i = g.node("ifft", "ifft");
    i.inputs["in"] = {events(2)};
    i.outputs["out"] = {audio(3)};
    g.node("out", "audioOut").inputs["in"] = {audio(3)};
    support::renderer r(2, 48000.0, 64);
    r.play(g.spec);
    const auto x = noise(1024);
    r.run(1024, {x});
    for (std::size_t t = 256; t < 1024; ++t) {
        CHECK(r.out(0)[t] == Approx(x[t - 256]).margin(1e-5));
        CHECK(r.out(1)[t] == Approx(0.0).margin(1e-6));
    }
}

namespace {

constexpr double rate = 48000.0;
constexpr int size = 1024;  // hop 256 at overlap 4

// Device input -> fft -> @p type with @p params -> ifft -> output 0. A
// sequencer on a metro, when @p steps is given, writes control pipe 5,
// which @p bindTo names a parameter of the processor to follow.
support::graph through(const std::string& type,
                       std::map<std::string, metadata::paramValue, std::less<>> params,
                       std::vector<double> steps = {}, double every = 1.0, const std::string& bindTo = "") {
    support::graph g;
    g.pipe(audio(0)).pipe(events(1)).pipe(events(2)).pipe(audio(3)).pipe(events(4)).pipe(support::control(5));
    g.node("in", "audioIn").outputs["out"] = {audio(0)};
    if (!steps.empty()) {
        auto& clock = g.node("clock", "metro");
        clock.params["every"] = every;
        clock.outputs["out"] = {events(4)};
        auto& seq = g.node("steps", "sequencer");
        seq.params["values"] = steps;
        seq.params["order"] = std::string("once");
        seq.inputs["in"] = {events(4)};
        seq.outputs["out"] = {support::control(5)};
        params[bindTo] = support::control(5);
    }
    auto& f = g.node("fft", "fft");
    f.params["size"] = static_cast<double>(size);
    f.inputs["in"] = {audio(0)};
    f.outputs["out"] = {events(1)};
    auto& p = g.node("proc", type);
    for (auto& [name, value] : params) p.params[name] = value;
    p.inputs["in"] = {events(1)};
    p.outputs["out"] = {events(2)};
    auto& i = g.node("ifft", "ifft");
    i.inputs["in"] = {events(2)};
    i.outputs["out"] = {audio(3)};
    g.node("out", "audioOut").inputs["in"] = {audio(3)};
    return g;
}

std::vector<float> run(const support::graph& g, const std::vector<float>& input) {
    support::renderer r(1, rate, 256);
    r.play(g.spec);
    r.run(static_cast<int>(input.size()), {input});
    return r.out(0);
}

// A tone burst: @p freq at 0.25 from @p from to @p to seconds, in @p seconds.
std::vector<float> burst(double freq, double from, double to, double seconds) {
    std::vector<float> x(static_cast<std::size_t>(seconds * rate), 0.0f);
    for (auto t = static_cast<std::size_t>(from * rate); t < static_cast<std::size_t>(to * rate); ++t) {
        x[t] = static_cast<float>(0.25 *
                                  std::sin(2.0 * std::numbers::pi * freq * static_cast<double>(t) / rate));
    }
    return x;
}

// The first sample at which |x| passes @p level.
std::size_t onset(const std::vector<float>& x, float level = 0.05f) {
    const auto it = std::ranges::find_if(x, [&](float v) { return std::abs(v) > level; });
    return static_cast<std::size_t>(it - x.begin());
}

}  // namespace

TEST_CASE("pvDelay at no delay passes frames and at n hops shifts them by n (samples.md 11.2)") {
    const auto x = noise(std::size_t{8} * size);
    const auto zero = run(through("pvDelay", {{"time", 0.0}}), x);
    for (std::size_t t = size; t < x.size(); ++t) CHECK(zero[t] == Approx(x[t - size]).margin(1e-5));
    // Three hops of 256 samples.
    const auto three = run(through("pvDelay", {{"time", 3.0 * 256.0 / rate}}), x);
    for (std::size_t t = size + 768; t < x.size(); ++t)
        CHECK(three[t] == Approx(x[t - size - 768]).margin(1e-5));
}

TEST_CASE("pvDelay's shapes delay low and high bins differently") {
    // A low and a high tone together, from 0.1 s; up to 0.3 s of delay.
    const auto low = burst(200.0, 0.1, 1.0, 1.0);
    const auto high = burst(15000.0, 0.1, 1.0, 1.0);
    const auto delayOf = [&](const std::string& shape, const std::vector<float>& x) {
        return static_cast<double>(onset(run(through("pvDelay", {{"time", 0.3}, {"shape", shape}}), x))) /
               rate;
    };
    // Each tone sounds at 0.1 s plus its bin's delay, 0.3 s times the
    // shape at the bin (k / 512 rising), plus the round trip's 1024 samples.
    const auto expected = [](double freq, bool rising) {
        const double position = freq * size / rate / (size / 2.0);
        return 0.1 + (0.3 * (rising ? position : 1.0 - position)) + (size / rate);
    };
    CHECK(delayOf("flat", low) == Approx(0.1 + 0.3 + (size / rate)).margin(0.01));
    CHECK(delayOf("flat", high) == Approx(0.1 + 0.3 + (size / rate)).margin(0.01));
    CHECK(delayOf("rising", low) == Approx(expected(200.0, true)).margin(0.01));
    CHECK(delayOf("rising", high) == Approx(expected(15000.0, true)).margin(0.01));
    CHECK(delayOf("falling", low) == Approx(expected(200.0, false)).margin(0.01));
    CHECK(delayOf("falling", high) == Approx(expected(15000.0, false)).margin(0.01));
}

TEST_CASE("pvDelay's random shape repeats for a seed and changes at a redraw") {
    const auto x = noise(std::size_t{16} * size);
    const auto g = through("pvDelay", {{"time", 0.05}, {"shape", std::string("random")}});
    CHECK(support::bitDifference(run(g, x), run(g, x)) == "");
    auto reseeded = g;
    reseeded.spec.seed = 9;
    CHECK(run(reseeded, x) != run(g, x));
    // A metro on :redraw draws new delays at each beat.
    auto redrawn = g;
    redrawn.pipe(events(6));
    auto& clock = redrawn.node("clock", "metro");
    clock.params["every"] = 0.125;
    clock.outputs["out"] = {events(6)};
    std::rotate(redrawn.spec.nodes.begin() + 1, redrawn.spec.nodes.end() - 1, redrawn.spec.nodes.end());
    for (auto& n : redrawn.spec.nodes) {
        if (n.id == "proc") n.inputs["redraw"] = {events(6)};
    }
    CHECK(run(redrawn, x) != run(g, x));
}

TEST_CASE("pvDelay's feedback stays bounded at 0.99") {
    // Quiet noise for a second, then silence; the output limiter is far off.
    auto x = noise(static_cast<std::size_t>(2 * rate));
    const auto loud = static_cast<std::size_t>(rate);
    for (std::size_t t = 0; t < x.size(); ++t) x[t] = t < loud ? x[t] * 0.002f : 0.0f;
    const auto y = run(through("pvDelay", {{"time", 0.05}, {"feedback", 0.99}}), x);
    CHECK(std::ranges::all_of(y, [](float v) { return std::isfinite(v); }));
    CHECK(support::peak(y) < 0.5);
    CHECK(support::peak(y, static_cast<std::size_t>(1.5 * rate)) >
          0.0);  // it still rings after the input stops
}

TEST_CASE("pvFreeze holds a tone at its frequency and level after the input stops (samples.md 11.3)") {
    // 1 kHz until 1 s; :hold rises at 0.75 s and falls at 1.5 s (a beat is
    // 0.5 s at 120 BPM).
    const auto x = burst(1000.0, 0.0, 1.0, 2.0);
    const auto y = run(through("pvFreeze", {{"fade", 0.05}}, {0, 1, 0}, 1.5, "hold"), x);
    const auto at = [](double seconds) { return static_cast<std::size_t>(seconds * rate); };
    const double input = support::rms(x, at(0.2), at(0.7));
    CHECK(support::rms(y, at(1.1), at(1.45)) == Approx(input).epsilon(0.12));  // within about 1 dB
    CHECK(support::pitch(y, at(1.1), at(1.45)) == Approx(1000.0).epsilon(0.01));
    // Released: it fades out within the fade and the round trip.
    CHECK(support::rms(y, at(1.65), at(2.0)) < 1e-3);
}

TEST_CASE("pvFreeze holds when its frames come blocks apart") {
    // A 4096-point fft at overlap 4: a frame every 1024 samples, four
    // 256-sample blocks, so :hold rises in a block without one.
    auto g = through("pvFreeze", {{"fade", 0.05}}, {0, 1}, 1.5, "hold");
    for (auto& n : g.spec.nodes) {
        if (n.id == "fft") n.params["size"] = 4096.0;
    }
    const auto x = burst(1000.0, 0.0, 1.0, 2.0);
    const auto y = run(g, x);
    const auto at = [](double seconds) { return static_cast<std::size_t>(seconds * rate); };
    CHECK(support::rms(y, at(1.3), at(1.9)) == Approx(support::rms(x, at(0.2), at(0.7))).epsilon(0.12));
}

TEST_CASE("pvBlur at 0 passes its input and near 1 smears it over time (samples.md 11.4)") {
    const auto x = noise(std::size_t{8} * size);
    const auto none = run(through("pvBlur", {{"amount", 0.0}}), x);
    for (std::size_t t = size; t < x.size(); ++t) CHECK(none[t] == Approx(x[t - size]).margin(1e-5));
    // A tone that drops 20 dB at 0.2 s: blurred, its level falls slowly.
    // (The phases are the input's, so a blur of silence is silent.)
    auto tone = burst(1000.0, 0.0, 1.0, 1.0);
    for (auto t = static_cast<std::size_t>(0.2 * rate); t < tone.size(); ++t) tone[t] *= 0.1f;
    const auto sharp = run(through("pvBlur", {{"amount", 0.0}}), tone);
    const auto smeared = run(through("pvBlur", {{"amount", 0.95}}), tone);
    const auto from = static_cast<std::size_t>(0.25 * rate);
    const auto to = static_cast<std::size_t>(0.3 * rate);
    CHECK(support::rms(sharp, from, to) == Approx(0.025 / std::sqrt(2.0)).epsilon(0.05));
    CHECK(support::rms(smeared, from, to) > 2.0 * support::rms(sharp, from, to));
    // Long after the step (amount 0.95 a hop is a time constant of about
    // 0.11 s), the blur settles at the new level.
    CHECK(support::rms(smeared, static_cast<std::size_t>(0.8 * rate), tone.size()) ==
          Approx(0.025 / std::sqrt(2.0)).epsilon(0.1));
}
