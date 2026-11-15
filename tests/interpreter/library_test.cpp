// The standard library: std/ imports and polyphony with voices
// (interpreter.md §11.2, primitives.md §7.1.5).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <string>

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
