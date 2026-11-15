// Loop gain through audio feedback pipes (design-notes "Audio hazards").

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

nodeSpec gainNode(std::string id, pipeRef in, double amount, pipeRef out) {
    return {std::move(id), "gain", {{"amount", amount}}, {{"in", {in}}}, {{"out", {out}}}, std::nullopt};
}

std::string text(const std::vector<ir::error>& found) {
    std::string out;
    for (const auto& e : found) out += e.text() + "\n";
    return out;
}

}  // namespace

TEST_CASE("a loop through three feedback pipes is found once") {
    graphSpec g;
    g.pipes = {{audio(0), 1, true}, {audio(1), 1, true}, {audio(2), 1, true}};
    g.nodes = {gainNode("a", audio(0), 2.0, audio(1)), gainNode("b", audio(2), 2.0, audio(0)),
               gainNode("c", audio(1), 2.0, audio(2))};
    REQUIRE(ir::validate(g).empty());
    const auto found = ir::hazards(g, 48000.0);
    INFO(text(found));
    REQUIRE(found.size() == 1);
    CHECK_THAT(found[0].message,
               ContainsSubstring("the loop through feedback audio pipes 0, 1, and 2 has a gain of 8"));

    // Below unity once around: 0.9 · 0.9 · 0.9.
    for (auto& n : g.nodes) n.params["amount"] = 0.9;
    CHECK(ir::hazards(g, 48000.0).empty());
}

TEST_CASE("a loop through a parameter bound to an audio pipe is found") {
    graphSpec g;
    const pipeRef fb = audio(0), src = audio(1), o = audio(2);
    g.pipes = {{fb, 1, true}, {src, 1, false}, {o, 1, false}};
    g.nodes.push_back({"s", "sineOsc", {}, {}, {{"out", {src}}}, std::nullopt});
    g.nodes.push_back({"d", "subtract", {{"b", fb}}, {{"in", {src}}}, {{"out", {o}}}, std::nullopt});
    g.nodes.push_back(gainNode("g", o, 3.0, fb));
    const auto found = ir::hazards(g, 48000.0);
    INFO(text(found));
    REQUIRE(found.size() == 1);
    CHECK_THAT(found[0].message, ContainsSubstring("the loop through feedback audio pipe 0 has a gain of 3"));

    // A parameter of unknown gain counts as 1: warned only above 1.
    g.nodes[1] = {"f", "sineOsc", {{"freq", fb}}, {}, {{"out", {o}}}, std::nullopt};
    const auto unknown = ir::hazards(g, 48000.0);
    INFO(text(unknown));
    REQUIRE(unknown.size() == 1);
    CHECK_THAT(unknown[0].message, ContainsSubstring("may have a gain of 3"));
}

TEST_CASE("a pipe given to mix twice counts each position's level") {
    graphSpec g;
    const pipeRef fb = audio(0), src = audio(1), o = audio(2);
    g.pipes = {{fb, 1, true}, {src, 1, false}, {o, 1, false}};
    g.nodes.push_back({"s", "sineOsc", {}, {}, {{"out", {src}}}, std::nullopt});
    g.nodes.push_back({"m",
                       "mix",
                       {{"levels", std::vector<double>{1.0, 0.3, 0.3}}},
                       {{"in", {src, fb, fb}}},
                       {{"out", {o}}},
                       std::nullopt});
    g.nodes.push_back(gainNode("g", o, 1.0, fb));
    CHECK(ir::hazards(g, 48000.0).empty());  // 0.3 + 0.3
    g.nodes[1].params["levels"] = std::vector<double>{1.0, 0.3, 0.8};
    const auto found = ir::hazards(g, 48000.0);
    REQUIRE(found.size() == 1);
    CHECK_THAT(found[0].message, ContainsSubstring("has a gain of 1.1"));
}
