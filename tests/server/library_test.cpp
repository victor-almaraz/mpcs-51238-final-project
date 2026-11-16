// The standard library's instruments and effects sound as documented
// (docs/library.md): pieces compiled from source and rendered offline.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string>
#include <vector>

#include <proj/interpreter/compiler.h>
#include <proj/ir/ir.h>

#include "support/graph.h"
#include "support/unwrap.h"

using Catch::Approx;
using support::crossings;
using support::peak;
using support::rms;

namespace {

constexpr int rate = 48000;
constexpr std::size_t second = rate;

// Renders @p body, a piece's body after the library imports, for
// @p seconds, with @p input on device input 0; returns output 0.
std::vector<float> render(const std::string& body, double seconds, std::vector<float> input = {}) {
    const auto compiled = interpreter::compile(
        "(import \"std/instruments\")\n(import \"std/effects\")\n(defpiece main ()\n" + body + ")\n",
        "test.lisp");
    INFO(compiled.text());
    REQUIRE(compiled.ok());
    REQUIRE(ir::validate(support::unwrap(compiled.graph)).empty());
    support::renderer r(1, rate, 64);
    r.play(support::unwrap(compiled.graph));
    const auto frames = static_cast<std::size_t>(seconds * rate);
    input.resize(frames, 0.0f);
    for (std::size_t at = 0; at < frames; at += 64) {
        r.run(64, {std::vector<float>(input.begin() + static_cast<std::ptrdiff_t>(at),
                                      input.begin() + static_cast<std::ptrdiff_t>(at + 64))});
    }
    const auto& out = r.out(0);
    REQUIRE(std::ranges::all_of(out, [](float v) { return std::isfinite(v); }));
    return out;
}

// One note of @p instrument: pitch @p note, held 0.5 s, from the start.
std::string oneNote(const std::string& instrument, int note) {
    return R"(
  (let ((clock (pipe :event)) (notes (pipe :event)) (out (pipe :audio)))
    (metro :every 64beat :out clock)
    (trigger-notes :in clock :pitch )" +
           std::to_string(note) + R"( :length 0.5s :velocity 1 :out notes)
    (play )" +
           instrument + R"( notes out)
    (audio-out :in out :channel 0)))";
}

// Brightness: how much the signal changes from sample to sample against
// its level; higher partials raise it.
double brightness(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double change = 0.0;
    double level = 0.0;
    for (std::size_t i = from + 1; i < to; ++i) {
        change += (x[i] - x[i - 1]) * (x[i] - x[i - 1]);
        level += x[i] * x[i];
    }
    return level > 0.0 ? std::sqrt(change / level) : 0.0;
}

// Upward crossings a second between @p from and @p to: the pitch of a
// tone whose fundamental dominates.
double perSecond(const std::vector<float>& x, std::size_t from, std::size_t to) {
    return crossings(x, from, to) * static_cast<double>(rate) / static_cast<double>(to - from);
}

// The pitch between @p from and @p to, 50 Hz to 2 kHz, by
// autocorrelation: the shortest lag whose correlation is within 10% of
// the strongest, so a tone rich in partials is not read an octave off.
double pitch(const std::vector<float>& x, std::size_t from, std::size_t to) {
    const std::size_t shortest = rate / 2000;
    const std::size_t longest = rate / 50;
    std::vector<double> r(longest + 1, 0.0);
    for (std::size_t lag = shortest; lag <= longest; ++lag) {
        for (std::size_t i = from; i + lag < to; ++i) r[lag] += x[i] * x[i + lag];
    }
    const double best = *std::ranges::max_element(r);
    for (std::size_t lag = shortest + 1; lag < longest; ++lag) {
        if (r[lag] >= 0.9 * best && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1])
            return static_cast<double>(rate) / static_cast<double>(lag);
    }
    return 0.0;
}

std::vector<float> sine(double freq, double seconds, double amplitude = 0.5) {
    std::vector<float> x(static_cast<std::size_t>(seconds * rate));
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(amplitude *
                                  std::sin(2.0 * std::numbers::pi * freq * static_cast<double>(i) / rate));
    return x;
}

// An effect on device input 0, written to output 0.
std::string effect(const std::string& make) {
    return R"(
  (let ((in (pipe :audio)) (out (pipe :audio)))
    (audio-in :channel 0 :out in)
    (process )" +
           make + R"( in out)
    (audio-out :in out :channel 0)))";
}

}  // namespace

TEST_CASE("subtractive plays the note's pitch, its filter closes after the attack, and it releases") {
    const auto x = render(oneNote("(make subtractive)", 57), 1.5);  // A3, 220 Hz
    CHECK(rms(x, second / 20, second / 2) > 0.02);
    CHECK(perSecond(x, second / 5, second / 2) == Approx(220.0).margin(8.0));
    CHECK(brightness(x, second / 50, second / 10) > 1.3 * brightness(x, second * 4 / 10, second / 2));
    CHECK(peak(x, second * 13 / 10) < 1e-3);  // 0.5 s held, 0.3 s release
}

TEST_CASE("fm sounds at the note's pitch and darkens as its modulation decays") {
    const auto x = render(oneNote("(make fm :ratio 1 :index 4)", 69), 1.0);  // A4
    CHECK(rms(x, second / 20, second / 2) > 0.02);
    CHECK(brightness(x, second / 100, second / 20) > 1.3 * brightness(x, second * 4 / 10, second / 2));
    CHECK(pitch(x, second * 4 / 10, second / 2) == Approx(440.0).margin(5.0));
}

TEST_CASE("pluck rings at the note's pitch and fades") {
    const auto x = render(oneNote("(make pluck)", 57), 2.0);
    CHECK(pitch(x, second / 10, second / 2) == Approx(220.0).margin(3.0));
    CHECK(rms(x, second / 10, second / 5) > 3.0 * rms(x, second * 3 / 2, second * 8 / 5));
    CHECK(rms(x, second * 3 / 2, second * 8 / 5) > 0.0);
}

TEST_CASE("an instrument plays as many notes at once as it has voices") {
    // Two notes at the same moment: one voice steals, two voices sum.
    const auto chord = [](int voices) {
        return R"(
  (let ((clock (pipe :event)) (notes (pipe :event)) (out (pipe :audio)))
    (metro :every 64beat :out clock)
    (trigger-notes :in clock :pitch 57 :length 0.5s :out notes)
    (trigger-notes :in clock :pitch 64 :length 0.5s :out notes)
    (play (make pluck :voices )" +
               std::to_string(voices) + R"() notes out)
    (audio-out :in out :channel 0)))";
    };
    const auto one = render(chord(1), 0.3);
    const auto two = render(chord(2), 0.3);
    CHECK(rms(two, second / 20, second / 4) > 1.3 * rms(one, second / 20, second / 4));
}

TEST_CASE("echo repeats at its time in beats, at the piece's tempo, each feedback times the last") {
    std::vector<float> click(second * 2, 0.0f);
    click[0] = 1.0f;
    // 0.5 beat at 60 BPM: every 24000 samples.
    const auto x = render(
        "(tempo 60bpm)" + effect("(make echo :time 0.5beat :feedback 0.5 :damping 0 :mix 1)"), 2.0, click);
    const auto at = [&](std::size_t centre) { return peak(x, centre - 200, centre + 200); };
    CHECK(at(24000) > 0.5);
    CHECK(at(48000) == Approx(0.5 * at(24000)).epsilon(0.05));
    CHECK(peak(x, 1000, 23000) < 1e-3);  // nothing between the repeats
}

TEST_CASE("an effect with a mix of 0 passes its input unchanged") {
    const auto in = sine(440.0, 0.5);
    const auto x = render(effect("(make reverb :mix 0)"), 0.5, in);
    double diff = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i)
        diff = std::max(diff, static_cast<double>(std::abs(x[i] - in[i])));
    CHECK(diff < 1e-5);
}

TEST_CASE("chorus sweeps its delay, so the mix of dry and delayed copies changes level") {
    const auto x = render(effect("(make chorus :rate 2hz :mix 0.5)"), 1.0, sine(2000.0, 1.0));
    double lowest = 1.0;
    double highest = 0.0;
    for (std::size_t at = second / 10; at + second / 50 <= second; at += second / 50) {
        const double level = rms(x, at, at + second / 50);
        lowest = std::min(lowest, level);
        highest = std::max(highest, level);
    }
    CHECK(highest > 1.5 * lowest);
}

TEST_CASE("reverb rings after an impulse, longer for a larger size, and decays") {
    std::vector<float> click(second * 4, 0.0f);
    click[0] = 1.0f;
    const auto small = render(effect("(make reverb :size 0.7 :mix 1)"), 4.0, click);
    const auto large = render(effect("(make reverb :size 0.9 :mix 1)"), 4.0, click);
    CHECK(rms(large, second / 10, second / 2) > 1e-3);
    CHECK(rms(large, second, second * 3 / 2) > 3.0 * rms(small, second, second * 3 / 2));
    CHECK(rms(small, second * 3, second * 4) < 1e-5);
}

TEST_CASE("overdrive saturates: its output is flatter-topped and below its level") {
    const auto x = render(effect("(make overdrive :drive 8 :level -6db :tone 10khz :mix 1)"), 0.5,
                          sine(220.0, 0.5, 1.0));
    const double top = peak(x, second / 10, second / 2);
    CHECK(top < 0.55);
    // A sine's RMS is 0.71 of its peak; a squarer wave's is nearer 1.
    CHECK(rms(x, second / 10, second / 2) / top > 0.85);
}
