// The standard library's instruments and effects sound as documented
// (docs/library.md): pieces compiled from source and rendered offline.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/ir/ir.h>

#include "support/graph.h"
#include "support/unwrap.h"

using Catch::Approx;
using support::crossings;
using support::peak;
using support::pitch;
using support::rms;
using support::sine;

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
    const auto frames = static_cast<int>(seconds * rate);
    input.resize(static_cast<std::size_t>(frames), 0.0f);
    r.run(frames, {input});
    return r.out(0);
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

TEST_CASE("sampler plays its buffer at each note's interval from its root (samples.md 7.3)") {
    // A 440 Hz sine recorded into the buffer as the piece starts; note 57,
    // an octave under the root, plays it at half speed, behind the writer.
    const std::string recorded = R"(
  (let ((ring (buffer :seconds 2s)) (tone (pipe :audio)))
    (sine-osc :freq 440hz :out tone)
    (buffer-write :buffer ring :in tone :mode :once)
    )";
    const auto x = render(
        recorded + oneNote("(make sampler :buffer ring :root 69 :voices 1 :release 50ms)", 57) + ")", 1.0);
    CHECK(pitch(x, second / 10, second / 2) == Approx(220.0).epsilon(0.01));
    // Sustain 1: the note holds the level of a full-scale sine at -12 dB.
    CHECK(rms(x, second / 10, second / 2) == Approx(0.25 / std::sqrt(2.0)).epsilon(0.05));
    // Released after 0.5 s.
    CHECK(rms(x, 6 * second / 10, second) < 1e-3);
}

TEST_CASE("an instrument plays as many notes at once as it has voices") {
    // Two notes at the same moment: one voice steals, two voices sum. A
    // subtractive voice has no noise, so the sum does not hang on a seed.
    const auto chord = [](int voices) {
        return R"(
  (let ((clock (pipe :event)) (notes (pipe :event)) (out (pipe :audio)))
    (metro :every 64beat :out clock)
    (trigger-notes :in clock :pitch 57 :length 0.5s :out notes)
    (trigger-notes :in clock :pitch 64 :length 0.5s :out notes)
    (play (make subtractive :voices )" +
               std::to_string(voices) + R"() notes out)
    (audio-out :in out :channel 0)))";
    };
    const auto one = render(chord(1), 0.3);
    const auto two = render(chord(2), 0.3);
    CHECK(rms(two, second / 20, second / 4) > 1.3 * rms(one, second / 20, second / 4));
}

TEST_CASE("echo repeats at its time in beats, at the piece's tempo, each feedback times the last") {
    std::vector<float> click(second * 11 / 10, 0.0f);
    click[0] = 1.0f;
    // 0.5 beat at 60 BPM: every 24000 samples.
    const auto x = render(
        "(tempo 60bpm)" + effect("(make echo :time 0.5beat :feedback 0.5 :damping 0 :mix 1)"), 1.1, click);
    const auto at = [&](std::size_t centre) { return peak(x, centre - 200, centre + 200); };
    CHECK(at(24000) > 0.5);
    CHECK(at(48000) == Approx(0.5 * at(24000)).epsilon(0.05));
    CHECK(peak(x, 1000, 23000) < 1e-3);  // nothing between the repeats
}

TEST_CASE(
    "granular-delay plays its input time later in grains and freezes when record is 0 (samples.md 8.4)") {
    // A 440 Hz tone for the first second, then silence.
    auto in = sine(440.0, 1.0);
    const auto x = render(effect("(make granular-delay :time 250ms :jitter 0s :mix 1)"), 2.0, in);
    CHECK(rms(x, 0, second / 5) < 1e-4);  // nothing recorded yet
    CHECK(rms(x, second / 2, 9 * second / 10) > 0.05);
    CHECK(pitch(x, second / 2, 9 * second / 10) == Approx(440.0).epsilon(0.02));
    CHECK(rms(x, 3 * second / 2, 2 * second) < 1e-4);  // the silence, recorded
    // Frozen at 1 s by a sequencer on :record: the grains keep reading the
    // tone 250 ms behind the held head.
    const auto frozen = render(R"(
  (let ((in (pipe :audio)) (out (pipe :audio)) (clock (pipe :event)) (record (pipe :control)))
    (audio-in :channel 0 :out in)
    (metro :every 2beat :out clock)
    (sequencer :in clock :values [1 0] :order :once :out record)
    (process (make granular-delay :jitter 0s :mix 1 :record record) in out)
    (audio-out :in out :channel 0)))",
                               2.0, in);
    CHECK(rms(frozen, 3 * second / 2, 2 * second) > 0.05);
    CHECK(pitch(frozen, 3 * second / 2, 2 * second) == Approx(440.0).epsilon(0.02));
}

TEST_CASE("spectral-delay plays its input time later and repeats it at its feedback (samples.md 11.5)") {
    auto in = sine(1000.0, 0.2);
    in.resize(second, 0.0f);
    const auto x =
        render(effect("(make spectral-delay :time 250ms :shape ':flat :feedback 0.5 :mix 1)"), 1.0, in);
    // 250 ms, plus the 2048 samples of the round trip: the first repeat
    // from about 0.29 s, the second, at half its level, from 0.54 s.
    const auto at = [](double s) { return static_cast<std::size_t>(s * rate); };
    CHECK(rms(x, 0, at(0.28)) < 1e-3);
    const double first = rms(x, at(0.34), at(0.44));
    const double repeat = rms(x, at(0.59), at(0.69));
    CHECK(first == Approx(0.5 / std::sqrt(2.0)).epsilon(0.1));
    CHECK(repeat == Approx(first * 0.5).epsilon(0.15));
}

TEST_CASE("freeze sustains the spectrum it holds after the input stops") {
    // A tone until 1 s; a sequencer raises :hold at 0.5 s.
    auto in = sine(1000.0, 1.0);
    const auto x = render(R"(
  (let ((in (pipe :audio)) (out (pipe :audio)) (clock (pipe :event)) (hold (pipe :control)))
    (audio-in :channel 0 :out in)
    (metro :every 1beat :out clock)
    (sequencer :in clock :values [0 1] :order :once :out hold)
    (process (make freeze :hold hold) in out)
    (audio-out :in out :channel 0)))",
                          2.0, in);
    CHECK(rms(x, static_cast<std::size_t>(1.3 * rate), 2 * second) ==
          Approx(0.5 / std::sqrt(2.0)).epsilon(0.12));
    CHECK(pitch(x, static_cast<std::size_t>(1.3 * rate), 2 * second) == Approx(1000.0).epsilon(0.01));
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
    const auto large = render(effect("(make reverb :size 0.9 :mix 1)"), 1.5, click);
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

TEST_CASE("reverb's wet signal is no louder than its input") {
    // Deterministic white noise at about 0.1 RMS.
    std::vector<float> noise(second);
    std::uint32_t state = 1;
    for (auto& v : noise) {
        state = (state * 1664525u) + 1013904223u;
        v = 0.173f * ((static_cast<float>(state >> 8u) / 8388608.0f) - 1.0f);
    }
    for (const std::string size : {"0.7", "0.84", "0.95"}) {
        INFO(size);
        const auto x = render(effect("(make reverb :size " + size + " :mix 1)"), 1.0, noise);
        CHECK(rms(x, second / 2, second) < rms(noise, second / 2, second));
    }
}

namespace {

// Renders a piece of the library imports, @p top, and the body @p body,
// for @p seconds, in a project with a page port, as chain's pedals need;
// returns outputs 0 and 1.
std::array<std::vector<float>, 2> renderStereo(const std::string& top, const std::string& body,
                                               double seconds) {
    interpreter::compileOptions options;
    options.pagePort = 8000;
    const auto compiled = interpreter::compile("(import \"std/instruments\")\n(import \"std/effects\")\n" +
                                                   top + "\n(defpiece main ()\n" + body + ")\n",
                                               "test.lisp", options);
    INFO(compiled.text());
    REQUIRE(compiled.ok());
    REQUIRE(ir::validate(support::unwrap(compiled.graph)).empty());
    support::renderer r(2, rate, 64);
    r.play(support::unwrap(compiled.graph));
    r.run(static_cast<int>(seconds * rate));
    return {r.out(0), r.out(1)};
}

}  // namespace

TEST_CASE("every effect runs as many channels as it writes, so a chain keeps a stereo signal's sides") {
    // A tone on the right only, through every standard effect, stays on
    // the right; granular-delay, whose grains are panned within its
    // spread of the centre, is not among them.
    const auto [left, right] = renderStereo("", R"(
  (let ((tone (pipe :audio)) (wide (pipe :audio :channels 2)) (out (pipe :audio :channels 2)))
    (sine-osc :freq 440hz :out tone)
    (pan :in tone :position 1 :out wide)
    (chain wide out (make chorus) (make echo :time 100ms) (make reverb) (make overdrive)
           (make spectral-delay) (make freeze :mix 0.5))
    (audio-out :in out :channel 0)))",
                                            1.0);
    CHECK(rms(right, second / 2, second) > 0.01);
    CHECK(rms(left, second / 2, second) < 1e-6);
}

TEST_CASE("chain asks each effect how many channels it writes, for the pipe to the next") {
    // An effect that widens a mono input to the right of two channels: the
    // reverb after it reads both, and the tone stays on the right.
    const auto [left, right] = renderStereo(R"(
(defclass widen (effect) ())
(defmethod output-channels ((w widen) in) 2)
(defmethod process ((w widen) in out) (pan :in in :position 1 :out out)))",
                                            R"(
  (let ((tone (pipe :audio)) (out (pipe :audio :channels 2)))
    (sine-osc :freq 440hz :out tone)
    (chain tone out (make widen :mix 1) (make reverb))
    (audio-out :in out :channel 0)))",
                                            1.0);
    CHECK(rms(right, second / 2, second) > 0.01);
    CHECK(rms(left, second / 2, second) < 1e-6);
}
