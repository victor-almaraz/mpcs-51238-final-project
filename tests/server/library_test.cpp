// The standard library's instruments and effects sound as documented
// (docs/library.md): pieces compiled from source and rendered offline.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <utility>
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

namespace {

// Renders a piece that imports the library and std/modulators, as
// renderStereo does; returns output 0.
std::vector<float> renderModulated(const std::string& body, double seconds) {
    return renderStereo("(import \"std/modulators\")", body, seconds)[0];
}

}  // namespace

TEST_CASE("a modulation moves a knob through its travel on the knob's scale (modulators.md 3.1)") {
    // An exponential knob from 100 Hz to 1600 Hz at 400 Hz, the middle of
    // its travel; a square LFO at amount 0.5 sweeps a quarter of the travel
    // either way: 800 Hz, then 200 Hz.
    const auto x = renderModulated(R"(
  (let ((f (knob "F" :min 100hz :max 1600hz :initial 400hz))
        (wobble (make lfo-mod :rate 0.5hz :shape ':square))
        (out (pipe :audio)))
    (sine-osc :freq (modulated f wobble 0.5) :out out)
    (audio-out :in out :channel 0)))",
                                   2.0);
    CHECK(pitch(x, second / 10, 9 * second / 10) == Approx(800.0).epsilon(0.01));
    CHECK(pitch(x, 11 * second / 10, 19 * second / 10) == Approx(200.0).epsilon(0.01));
}

TEST_CASE("a modulation of a level moves it in decibels, and a unipolar one only upward") {
    // A level knob from 0 at -20 dB sits at two thirds of a travel from
    // -60 dB to 0 dB; a square LFO at amount 0.5 moves it to -5 dB and to
    // -35 dB. Unipolar at 0.25, it moves to -5 dB and back to -20 dB.
    const auto levels = [](const std::string& modulation) {
        const auto x = renderModulated(R"(
  (let ((l (knob "Level" :max 0db :initial -20db))
        (wobble (make lfo-mod :rate 0.5hz :shape ':square))
        (tone (pipe :audio)) (out (pipe :audio)))
    (sine-osc :freq 440hz :out tone)
    (gain :in tone :amount )" + modulation +
                                           R"( :out out)
    (audio-out :in out :channel 0)))",
                                       2.0);
        const auto db = [](double r) { return 20.0 * std::log10(r * std::numbers::sqrt2); };
        return std::pair{db(rms(x, second / 10, 9 * second / 10)),
                         db(rms(x, 11 * second / 10, 19 * second / 10))};
    };
    const auto [high, low] = levels("(modulated l wobble 0.5)");
    CHECK(high == Approx(-5.0).margin(0.1));
    CHECK(low == Approx(-35.0).margin(0.1));
    const auto [up, rest] = levels("(modulated l wobble 0.25 :polarity ':unipolar)");
    CHECK(up == Approx(-5.0).margin(0.1));
    CHECK(rest == Approx(-20.0).margin(0.1));
}

TEST_CASE("modulate moves a drawn knob in a chain and an instrument, the knob still on the page") {
    // A pedal's mix knob at 0 moved by a square LFO at amount 2: wet for
    // half a cycle, dry for the other (the position kept within the travel).
    const auto x = renderModulated(R"(
  (let ((wobble (make lfo-mod :rate 0.5hz :shape ':square))
        (tone (pipe :audio)) (out (pipe :audio)))
    (sine-osc :freq 440hz :out tone)
    (page (chain tone out (modulate (make overdrive :mix 0 :level -20db) 'mix wobble 2)))
    (audio-out :in out :channel 0)))",
                                   2.0);
    const double wet = rms(x, second / 10, 9 * second / 10);
    const double dry = rms(x, 11 * second / 10, 19 * second / 10);
    CHECK(dry == Approx(1.0 / std::numbers::sqrt2).epsilon(0.01));
    CHECK(wet < 0.5 * dry);
}

TEST_CASE("each modulator moves a knob: random, envelope, follower, steps, and a rate in beats") {
    // A knob from 0 to 1 at 0, moved unipolar by amount 1, reads the
    // modulator's signal as the level of a tone.
    const auto level = [](const std::string& setup, const std::string& source, double from, double to) {
        const auto x = renderModulated(R"(
  (let ((l (knob "L" :initial 0))
        (tone (pipe :audio)) (out (pipe :audio)))
    )" + setup + R"(
    (sine-osc :freq 440hz :out tone)
    (gain :in tone :amount (modulated l )" +
                                           source +
                                           R"( 1 :polarity ':unipolar) :out out)
    (audio-out :in out :channel 0)))",
                                       2.0);
        return rms(x, static_cast<std::size_t>(from * rate), static_cast<std::size_t>(to * rate)) *
               std::numbers::sqrt2;
    };
    // Random: held values within 0 to 1 that change at the rate, measured
    // where each has finished gliding (50 ms after the 0.25 s steps).
    const double a = level("", "(make random-mod :rate 4hz)", 0.33, 0.49);
    const double b = level("", "(make random-mod :rate 4hz)", 0.58, 0.74);
    CHECK(a >= 0.0);
    CHECK(a <= 1.0);
    CHECK(b >= 0.0);
    CHECK(b <= 1.0);
    CHECK(a != Approx(b).margin(1e-3));
    // Envelope: silent before a note, at its sustain while it is held.
    const std::string note = R"((define notes (let ((clock (pipe :event)) (n (pipe :event)))
      (metro :every 64beat :out clock) (trigger-notes :in clock :pitch 60 :length 1s :out n) n)))";
    CHECK(level(note, "(make envelope-mod :notes notes :attack 1ms :decay 10ms :sustain 0.5)", 0.5, 0.9) ==
          Approx(0.5).margin(0.01));
    // Follower: the level of a tone at full scale, about 0.64 of its peak.
    const std::string loud = "(define loud (pipe :audio)) (sine-osc :freq 220hz :out loud)";
    CHECK(level(loud, "(make follower :in loud)", 0.5, 1.5) == Approx(2.0 / std::numbers::pi).margin(0.03));
    // Steps: each value in order, the first from the start, then around
    // again, the rate a knob on the page.
    const std::string steps =
        "(define st (make step-mod :rate 2hz :steps [0.1 0.4 0.8])) (page (section \"Steps\" st) l)";
    CHECK(level(steps, "st", 0.05, 0.45) == Approx(0.1).margin(0.01));
    CHECK(level(steps, "st", 0.55, 0.95) == Approx(0.4).margin(0.01));
    CHECK(level(steps, "st", 1.05, 1.45) == Approx(0.8).margin(0.01));
    CHECK(level(steps, "st", 1.55, 1.95) == Approx(0.1).margin(0.01));
    // A rate of one cycle a beat follows the tempo, 1 Hz at 60 BPM (not the
    // 120 BPM default): a square LFO is up for half a second, then down.
    const std::string slow = "(tempo 60bpm)";
    CHECK(level(slow, "(make lfo-mod :rate (beat-rate 1beat) :shape ':square)", 0.05, 0.45) ==
          Approx(1.0).margin(0.01));
    CHECK(level(slow, "(make lfo-mod :rate (beat-rate 1beat) :shape ':square)", 0.55, 0.95) ==
          Approx(0.0).margin(0.01));
}

TEST_CASE("modulate moves an instrument's drawn knob: a cutoff swept by a square LFO brightens and darkens") {
    // A held note through a subtractive voice whose cutoff knob, 400 Hz on
    // a travel of 50 Hz to 8 kHz, a square LFO at amount 0.6 moves up for
    // a second and down for the next.
    const auto x = renderModulated(R"(
  (let ((synth (make subtractive :voices 1 :cutoff 400hz :env-amount 0hz :sustain 1))
        (wobble (make lfo-mod :rate 0.5hz :shape ':square))
        (clock (pipe :event)) (notes (pipe :event)) (out (pipe :audio)))
    (modulate synth 'cutoff wobble 0.6)
    (page (synth-panel "S" synth))
    (metro :every 64beat :out clock)
    (trigger-notes :in clock :pitch 45 :length 4s :out notes)
    (play synth notes out)
    (audio-out :in out :channel 0)))",
                                   2.0);
    CHECK(brightness(x, second / 5, 9 * second / 10) > 2.0 * brightness(x, 6 * second / 5, 19 * second / 10));
}
