// The standard library: std/ imports and polyphony with voices
// (interpreter.md §11.2, primitives.md §7.1.5).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <string>

#include <proj/interpreter/compiler.h>
#include <proj/metadata/graph.h>

#include "support/compile.h"
#include "support/unwrap.h"

using Catch::Matchers::ContainsSubstring;
using support::error;
using support::find;
using support::ok;
using support::piece;

TEST_CASE("voices splits notes among voices and builds each one, keyed by its number") {
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
        // voices@<call>/fn@<its build call>#<voice>/adsr@...: one keyed step per voice.
        CHECK_THAT(env.id, ContainsSubstring("/main/voices@6:5#0/fn@"));
        CHECK_THAT(env.id, ContainsSubstring("#" + std::to_string(v) + "/adsr@6:29#0"));
    }
    CHECK(support::unwrap(find(g, "voiceSplit").src).file == "std/voices.lisp");
}

TEST_CASE("std/ names only the standard library") {
    CHECK_THAT(error(piece("1", "(import \"std/drums\")")),
               ContainsSubstring("cannot import std/drums.lisp: the standard library has no such file"));
}

TEST_CASE("nth and length read vectors") {
    const auto g =
        ok(piece("(lfo :offset (+ (nth [5 6 7] 2) (length [1 2]) (length nil)) :out (pipe :control))"));
    CHECK(std::get<double>(find(g, "lfo").params.at("offset")) == 9.0);
    CHECK_THAT(error(piece("(nth [1] 1)")), ContainsSubstring("index 1 is outside a vector of 1 items"));
    CHECK_THAT(error(piece("(nth 1 0)")), ContainsSubstring("expected (nth vector index)"));
    CHECK_THAT(error(piece("(length 3)")), ContainsSubstring("expected (length vector)"));
}

TEST_CASE("unit-value gives a quantity as a plain number in a unit (grammar.md 5.5)") {
    const auto value = [](const std::string& expr) {
        const auto g = ok(piece("(lfo :offset " + expr + " :out (pipe :control))"));
        return std::get<double>(find(g, "lfo").params.at("offset"));
    };
    CHECK(value("(unit-value 250ms 's)") == Catch::Approx(0.25));
    CHECK(value("(unit-value 1.5khz 'hz)") == Catch::Approx(1500.0));
    CHECK(value("(unit-value -6db 'db)") == Catch::Approx(-6.0));
    CHECK(value("(unit-value 2bar 'beat)") == Catch::Approx(8.0));
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
    CHECK(find(ok(withLibrary("(play (make subtractive :voices 3) notes wet)")), "voiceSplit")
              .outputs.at("out")
              .size() == 3);
    const auto sub = ok(withLibrary("(play (make subtractive :voices 3 :wave ':pulse) notes wet)"));
    CHECK_NOTHROW(find(sub, "ladder", 2));
    CHECK_NOTHROW(find(sub, "pulseOsc", 2));
    const auto fm = ok(withLibrary("(play (make fm :voices 2 :ratio 3) notes wet)"));
    CHECK_NOTHROW(find(fm, "sineOsc", 3));  // a modulator and a carrier per voice
    const auto pluck = ok(withLibrary("(play (make pluck :voices 2) notes wet)"));
    CHECK_NOTHROW(find(pluck, "comb", 1));
}

TEST_CASE("an effect's :around method mixes its wet signal with the dry input") {
    const auto g = ok(withLibrary("(play (make pluck) notes dry) (process (make echo :mix 0.25) dry wet)"));
    const auto& mix = find(g, "crossfade");
    CHECK(std::get<double>(mix.params.at("position")) == 0.25);
    // a is the dry pluck (its 8 voices' combs); b is the echo's comb.
    CHECK(mix.inputs.at("a") == find(g, "comb", 0).outputs.at("out"));
    CHECK(mix.inputs.at("b") == find(g, "comb", 8).outputs.at("out"));
    CHECK(mix.outputs.at("out").front() == find(g, "audioOut").inputs.at("in").front());
    // The echo's time is in beats, so it follows the tempo.
    CHECK_NOTHROW(find(g, "beatsToSeconds"));
    for (const std::string effect : {"chorus", "reverb", "overdrive"}) {
        CHECK(ok(withLibrary("(play (make fm) notes dry) (process (make " + effect + ") dry wet)"))
                  .nodes.size() > 10);
    }
}
