// Primitive constraints that span several parameters and ports
// (metadata constraint tables), checked by IR validation.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <proj/ir/ir.h>

using Catch::Matchers::ContainsSubstring;
using metadata::graphSpec;
using metadata::nodeSpec;
using metadata::pipeKind;
using metadata::pipeRef;

namespace {

constexpr pipeRef audio(std::int32_t i) { return {pipeKind::audio, i}; }
constexpr pipeRef control(std::int32_t i) { return {pipeKind::control, i}; }
constexpr pipeRef event(std::int32_t i) { return {pipeKind::event, i}; }

// Mono audio 0 and 1, stereo audio 2, control 1, event 0; @p n reads and
// writes some of them.
graphSpec with(nodeSpec n) {
    graphSpec g;
    g.pipes = {{audio(0), 1, false},
               {audio(1), 1, false},
               {audio(2), 2, false},
               {control(1), 1, false},
               {event(0), 1, false}};
    g.nodes.push_back(std::move(n));
    return g;
}

std::string errors(const graphSpec& g) {
    std::string out;
    for (const auto& e : ir::validate(g)) out += e.text() + "\n";
    return out;
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
                            {{"in", {event(0)}}},
                            {{"out", {control(1)}}},
                            std::nullopt})),
               ContainsSubstring("parameter values must not be empty"));
    CHECK_THAT(
        errors(with({"q", "sequencer", {}, {{"in", {event(0)}}}, {{"out", {control(1)}}}, std::nullopt})),
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
