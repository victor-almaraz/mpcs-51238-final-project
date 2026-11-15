// Primitive constraints that span several parameters and ports
// (metadata constraint tables), checked by IR validation.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <proj/metadata/graph.h>

#include "support/pipes.h"
#include "support/validation.h"

using Catch::Matchers::ContainsSubstring;
using metadata::graphSpec;
using metadata::nodeSpec;
using support::audio;
using support::control;
using support::errors;
using support::events;

namespace {

// Mono audio 0 and 1, stereo audio 2, control 1, event 0; @p n reads and
// writes some of them.
graphSpec with(nodeSpec n) {
    graphSpec g;
    g.pipes = {{.pipe = audio(0), .channels = 1, .feedback = false},
               {.pipe = audio(1), .channels = 1, .feedback = false},
               {.pipe = audio(2), .channels = 2, .feedback = false},
               {.pipe = control(1), .channels = 1, .feedback = false},
               {.pipe = events(0), .channels = 1, .feedback = false}};
    g.nodes.push_back(std::move(n));
    return g;
}

}  // namespace

TEST_CASE("validation checks the constraints the engine relies on") {
    CHECK_THAT(errors(with({"m",
                            "mix",
                            {{"levels", std::vector<double>{0.5}}},
                            {{"in", {audio(0), audio(1)}}},
                            {{"out", {audio(2)}}},
                            std::nullopt})),
               ContainsSubstring("parameter levels has 1 entry for 2 pipes on input in"));
    CHECK(errors(with({"m",
                       "mix",
                       {{"levels", std::vector<double>{}}},
                       {{"in", {audio(0), audio(1)}}},
                       {{"out", {audio(2)}}},
                       std::nullopt}))
              .empty());  // empty levels are all 1
    CHECK_THAT(errors(with({"q",
                            "sequencer",
                            {{"values", std::vector<double>{}}},
                            {{"in", {events(0)}}},
                            {{"out", {control(1)}}},
                            std::nullopt})),
               ContainsSubstring("parameter values must not be empty"));
    CHECK_THAT(
        errors(with({"q", "sequencer", {}, {{"in", {events(0)}}}, {{"out", {control(1)}}}, std::nullopt})),
        ContainsSubstring("parameter values must not be empty"));
    CHECK_THAT(errors(with({"f", "svf", {}, {{"in", {audio(0)}}}, {}, std::nullopt})),
               ContainsSubstring("needs at least one of output out, low, band, high, notch"));
    CHECK_THAT(errors(with({"i", "oscIn", {}, {}, {}, std::nullopt})),
               ContainsSubstring("needs at least one of output out, value"));
    CHECK_THAT(errors(with({"o", "oscOut", {}, {}, {}, std::nullopt})),
               ContainsSubstring("needs at least one of input in, value"));
    CHECK_THAT(
        errors(
            with({"o", "oscOut", {{"address", std::string()}}, {{"value", {control(1)}}}, {}, std::nullopt})),
        ContainsSubstring("with input value, parameter address must not be empty"));
    CHECK(errors(with({"o", "oscOut", {}, {{"value", {control(1)}}}, {}, std::nullopt}))
              .empty());  // default /value
    CHECK_THAT(errors(with({"p", "pan", {}, {{"in", {audio(0)}}}, {{"out", {audio(1)}}}, std::nullopt})),
               ContainsSubstring("output out needs a pipe with at least 2 channels"));
    CHECK(errors(with({"p", "pan", {}, {{"in", {audio(0)}}}, {{"out", {audio(2)}}}, std::nullopt})).empty());
    CHECK_THAT(errors(with({"e", "adsr", {}, {{"in", {audio(0)}}}, {{"out", {control(1)}}}, std::nullopt})),
               ContainsSubstring("with input in, output out must be an audio pipe"));
    CHECK(errors(with({"e", "adsr", {}, {}, {{"out", {control(1)}}}, std::nullopt})).empty());
}
