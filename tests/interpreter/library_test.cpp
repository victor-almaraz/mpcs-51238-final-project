// The standard library: std/ imports and polyphony with voices
// (interpreter.md §11.2, primitives.md §7.1.5).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/metadata/graph.h>

#include "support/compile.h"
#include "support/pipes.h"
#include "support/unwrap.h"

using Catch::Matchers::ContainsSubstring;
using support::audio;
using support::count;
using support::error;
using support::find;
using support::ok;
using support::piece;
using support::valueOf;

TEST_CASE("voices splits notes among voices and builds each one, numbered by voice") {
    const auto g = ok(piece(R"(
  (let ((notes (pipe :event)) (out (pipe :audio)))
    (midi-in :device "" :out notes)
    (voices 3 notes (fn (n) (adsr :gate n :in (pipe :audio) :out out)))
    (audio-out :in out :channel 0)))",
                            "(import \"std/voices\")"));
    const auto& split = find(g, "voiceSplit");
    REQUIRE(split.outputs.at("out").size() == 3);
    for (std::size_t v = 0; v < 3; ++v) {
        const auto& env = find(g, "adsr", v);
        CHECK(env.inputs.at("gate").front() == split.outputs.at("out")[v]);
        // The anonymous build adds no step; the voice number is the adsr's occurrence.
        CHECK(env.id == "test.lisp/main/voices@6:5#0/adsr@6:29#" + std::to_string(v));
    }
    CHECK(support::unwrap(split.src).file == "std/voices.lisp");
}

TEST_CASE("std/ names only the standard library") {
    CHECK_THAT(error(piece("1", "(import \"std/drums\")")),
               ContainsSubstring("cannot import std/drums.lisp: the standard library has no such file"));
}

TEST_CASE("nth and length read vectors") {
    CHECK(valueOf("(+ (nth [5 6 7] 2) (length [1 2]) (length nil))") == 9.0);
    CHECK_THAT(error(piece("(nth [1] 1)")), ContainsSubstring("index 1 is outside a vector of 1 items"));
    CHECK_THAT(error(piece("(nth 1 0)")), ContainsSubstring("expected (nth vector index)"));
    CHECK_THAT(error(piece("(length 3)")), ContainsSubstring("expected (length vector)"));
}

TEST_CASE("an index or a count is an integer or a real with an integer value") {
    // floor, round, and / give reals; an index computed with them still indexes.
    CHECK(valueOf("(nth [5 6 7] (floor (/ 5 2)))") == 7.0);
    CHECK(valueOf("(nth [5 6 7] 1.0)") == 6.0);
    CHECK(valueOf("(length (range (round 2.6)))") == 3.0);
    CHECK(valueOf("(length (range 0.0))") == 0.0);
    CHECK(ok(piece("(pipe :audio :channels (/ 4 2))")).pipes.at(0).channels == 2);

    CHECK_THAT(error(piece("(nth [5 6 7] 1.5)")),
               ContainsSubstring("nth: the index must be an integer, got 1.5"));
    CHECK_THAT(error(piece("(nth [5 6 7] 1s)")), ContainsSubstring("nth: the index must be an integer"));
    CHECK_THAT(error(piece("(nth [5 6 7] 'a)")),
               ContainsSubstring("nth: the index must be an integer, got a"));
    CHECK_THAT(error(piece("(nth [5 6 7] 3.0)")),
               ContainsSubstring("index 3 is outside a vector of 3 items"));
    CHECK_THAT(error(piece("(nth [5 6 7] (- 1.0))")),
               ContainsSubstring("index -1 is outside a vector of 3 items"));
    CHECK_THAT(error(piece("(nth [5] 1e300)")), ContainsSubstring("nth: the index must be an integer"));
    CHECK_THAT(error(piece("(range 2.5)")),
               ContainsSubstring("range: n must be an integer from 0 to 100000, got 2.5"));
    CHECK_THAT(error(piece("(range -1)")),
               ContainsSubstring("range: n must be an integer from 0 to 100000, got -1"));
    CHECK_THAT(error(piece("(range 1 2)")), ContainsSubstring("expected (range n)"));
    CHECK_THAT(error(piece("(pipe :audio :channels 1.5)")),
               ContainsSubstring("channels must be an integer from 1 to"));
}

TEST_CASE("unit-value gives a quantity as a plain number in a unit (grammar.md 5.5)") {
    CHECK(valueOf("(unit-value 250ms 's)") == Catch::Approx(0.25));
    CHECK(valueOf("(unit-value 1.5khz 'hz)") == Catch::Approx(1500.0));
    CHECK(valueOf("(unit-value -6db 'db)") == Catch::Approx(-6.0));
    CHECK(valueOf("(unit-value 2bar 'beat)") == Catch::Approx(8.0));
    CHECK_THAT(error(piece("(unit-value 250ms 'hz)")),
               ContainsSubstring("unit-value: hz measures a frequency, got a time"));
    CHECK_THAT(error(piece("(unit-value 250ms 'parsec)")), ContainsSubstring("unknown unit parsec"));
}

namespace {

// A piece that plays notes from a sequencer through @p build, after
// importing the instruments and effects.
interpreter::compileResult withLibrary(const std::string& build) {
    return piece(R"(
  (let ((clock (pipe :event)) (seq (pipe :event)) (notes (pipe :event))
        (dry (pipe :audio)) (wet (pipe :audio)))
    (metro :every 0.5beat :out clock)
    (sequencer :in clock :values [60 64 67] :events seq)
    (trigger-notes :in seq :length 0.25beat :out notes)
    )" + build + R"(
    (audio-out :in wet :channel 0)))",
                 "(import \"std/instruments\")\n(import \"std/effects\")");
}

}  // namespace

TEST_CASE("each instrument builds one voice per voice count") {
    const auto sub = ok(withLibrary("(play (make subtractive :voices 3 :wave ':pulse) notes wet)"));
    CHECK(find(sub, "voiceSplit").outputs.at("out").size() == 3);
    CHECK(count(sub, "ladder") == 3);
    CHECK(count(sub, "pulseOsc") == 3);
    CHECK(count(sub, "gain") == 1);  // the level, once after the voices sum
    const auto fm = ok(withLibrary("(play (make fm :voices 2 :ratio 3) notes wet)"));
    CHECK(count(fm, "sineOsc") == 4);  // a modulator and a carrier per voice
    const auto pluck = ok(withLibrary("(play (make pluck :voices 2) notes wet)"));
    CHECK(count(pluck, "comb") == 2);
}

TEST_CASE("an effect's :around method mixes its wet signal with the dry input") {
    const auto g = ok(withLibrary("(play (make pluck) notes dry) (process (make echo :mix 0.25) dry wet)"));
    const auto& mix = find(g, "crossfade");
    CHECK(std::get<double>(mix.params.at("position")) == 0.25);
    // a is the dry pipe, which the pluck plays into; b is the echo's comb, after the pluck's 8.
    CHECK(mix.inputs.at("a") == std::vector{audio(0)});
    CHECK(mix.inputs.at("b") == find(g, "comb", 8).outputs.at("out"));
    CHECK(mix.outputs.at("out") == std::vector{audio(1)});
    // The notes' length and the echo's time are in beats, so they follow the tempo.
    CHECK(count(g, "beatsToSeconds") == 2);
    // Each effect's own nodes, after an instrument that has none of them.
    const auto with = [](const std::string& effect) {
        return ok(
            withLibrary("(play (make fm :voices 1) notes dry) (process (make " + effect + ") dry wet)"));
    };
    const auto chorus = with("chorus");
    CHECK((count(chorus, "lfo") == 1 && count(chorus, "delayLine") == 1 && count(chorus, "crossfade") == 1));
    const auto reverb = with("reverb");
    CHECK((count(reverb, "comb") == 4 && count(reverb, "allpass") == 2 && count(reverb, "crossfade") == 1));
    const auto overdrive = with("overdrive");
    CHECK((count(overdrive, "tanh") == 1 && count(overdrive, "biquad") == 1 &&
           count(overdrive, "crossfade") == 1));
}

TEST_CASE("a sampler of a stereo buffer plays and sums its voices in two channels") {
    const auto g = ok(piece(R"(
  (let ((ring (buffer :channels 2 :seconds 1s)) (notes (pipe :event)) (out (pipe :audio :channels 2)))
    (play (make sampler :buffer ring :voices 3 :loop ':forward) notes out)))",
                            "(import \"std/instruments\")"));
    CHECK(count(g, "samplePlayer") == 3);
    const auto& player = find(g, "samplePlayer");
    CHECK(std::get<std::string>(player.params.at("loop")) == "forward");
    CHECK(std::get<double>(player.params.at("root")) == 60.0);
    const auto channelsOf = [&](metadata::pipeRef r) {
        return support::unwrap(metadata::findPipe(g, r)).get().channels;
    };
    CHECK(channelsOf(player.outputs.at("out").front()) == 2);
    CHECK(channelsOf(find(g, "gain").inputs.at("in").front()) == 2);
}
