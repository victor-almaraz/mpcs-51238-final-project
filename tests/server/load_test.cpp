// The load piece of the v2 load test (threads.md §1.2, §11 step 1): it
// compiles, keeps its 64 voices sounding, and profiles by node.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>
#include <espresso/primitives/engine.h>

#include "offline.h"
#include "support/graph.h"
#include "support/unwrap.h"

using Catch::Matchers::ContainsSubstring;

namespace {

constexpr double rate = 48000.0;
constexpr int block = 256;

// The load piece, with @p layers layers a voice when given; the piece's own
// count otherwise.
metadata::graphSpec loadPiece(std::optional<int> layers = std::nullopt) {
    std::ifstream in(std::filesystem::path(ESPRESSO_SOURCE_DIR) / "tests" / "server" / "load" / "load.lisp");
    std::stringstream text;
    text << in.rdbuf();
    auto source = text.str();
    if (layers) {
        const std::string_view line = "(define layers 8)";
        const auto at = source.find(line);
        REQUIRE(at != std::string::npos);
        source.replace(at, line.size(), "(define layers " + std::to_string(*layers) + ")");
    }
    const auto compiled = interpreter::compile(source, "load.lisp");
    INFO(compiled.text());
    REQUIRE(compiled.ok());
    auto g = support::unwrap(compiled.graph);
    REQUIRE(ir::validate(g).empty());
    return g;
}

std::size_t countOf(const metadata::graphSpec& g, std::string_view type) {
    return static_cast<std::size_t>(std::ranges::count(g.nodes, type, &metadata::nodeSpec::type));
}

}  // namespace

TEST_CASE("the load piece has 64 layered voices through 16 effects, profiled by node") {
    const auto g = loadPiece();
    CHECK(countOf(g, "voiceSplit") == 8);
    CHECK(countOf(g, "noteToFreq") == 64);  // a voice reads its notes' pitch once
    // Eight layers a voice: a ladder each in the three stacks, a comb each in the two strings.
    CHECK(countOf(g, "ladder") == 3 * 8 * 8);
    CHECK(countOf(g, "comb") >= 2 * 8 * 8);

    support::renderer r({.audio = {.sampleRate = rate, .blockSize = block}, .pipes = {}, .profile = true}, 2);
    const auto plan = r.engine.build(g, 1);
    r.engine.play(plan, 0.0, primitives::quantization::none);
    r.run(block * 16);
    const double total = std::accumulate(plan->seconds.begin(), plan->seconds.end(), 0.0);
    CHECK(total > 0.0);
    const auto report = server::profileReport(*plan, 16.0 * block / rate);
    CHECK_THAT(report, ContainsSubstring("profile: " + std::to_string(plan->nodes.size()) + " nodes"));
    CHECK_THAT(report, ContainsSubstring("ladder"));
    CHECK_THAT(report, ContainsSubstring("slowest nodes:"));
}

TEST_CASE("the load piece keeps its 64 voices sounding") {
    // One layer a voice: the same notes and voices at a cost the Debug suite can render.
    support::renderer r(2, rate, block);
    const auto plan = r.engine.build(loadPiece(1), 1);
    r.engine.play(plan, 0.0, primitives::quantization::none);
    for (int i = 0; i < static_cast<int>(2.5 * rate) / block; ++i) r.run(block);
    // Every voice sounds after two beats at 120 BPM (1 s); the mix holds its level.
    const auto& left = r.out(0);
    CHECK(support::rms(left, left.size() - 24000, left.size()) > 0.02);
}

TEST_CASE("an engine that does not profile leaves the plan's node times at zero") {
    support::renderer r(2, rate, block);
    const auto plan = r.engine.build(loadPiece(), 1);
    r.engine.play(plan, 0.0, primitives::quantization::none);
    r.run(block * 4);
    CHECK(std::accumulate(plan->seconds.begin(), plan->seconds.end(), 0.0) == 0.0);
}

TEST_CASE("report: the load piece's tasks", "[.bench]") {
    support::renderer r(2, rate, block);
    const auto plan = r.engine.build(loadPiece(), 1);
    const auto& t = plan->tasks;
    // Longest path by estimated cost, in an order where every task follows
    // those it waits for.
    std::vector<std::uint32_t> left = t.waits;
    std::vector<double> finish(t.size(), 0.0);
    std::vector<std::uint32_t> ready;
    for (std::uint32_t i = 0; i < t.size(); ++i) {
        if (left[i] == 0) ready.push_back(i);
    }
    double longest = 0.0;
    while (!ready.empty()) {
        const auto i = ready.back();
        ready.pop_back();
        finish[i] += t.cost[i];
        longest = std::max(longest, finish[i]);
        for (const auto u : t.successorsOf(i)) {
            finish[u] = std::max(finish[u], finish[i]);
            if (--left[u] == 0) ready.push_back(u);
        }
    }
    const double work = std::accumulate(t.cost.begin(), t.cost.end(), 0.0);
    auto sorted = t.cost;
    std::ranges::sort(sorted);
    std::cout << "tasks " << t.size() << " of " << plan->nodes.size() << " nodes; estimated work "
              << work / 1000.0 << " us a block; median task " << sorted[sorted.size() / 2] / 1000.0
              << " us, largest " << sorted.back() / 1000.0 << " us; critical path " << longest / 1000.0
              << " us (work / path " << work / longest << ")\n";
    CHECK(longest > 0.0);
}
