// Tasks: chains, joining small tasks of one level, and the property test
// of threads.md §9.2 on random dependencies.

#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string_view>
#include <vector>

#include <espresso/primitives/plan.h>

#include "../../src/primitives/costs.h"
#include "../../src/primitives/tasks.h"
#include "support/tasks.h"

using primitives::taskList;

namespace {

std::vector<std::uint32_t> nodesOf(const taskList& t, std::size_t i) {
    const auto n = t.nodesOf(i);
    return {n.begin(), n.end()};
}

}  // namespace

TEST_CASE("a chain becomes one task, however costly") {
    // 0 -> 1 -> 2, each node the only follower of the one before.
    const auto deps = support::listOf({{}, {0}, {1}});
    const std::vector<double> cost{1e6, 1e6, 1e6};
    const auto t = primitives::tasksOf(deps, cost);
    REQUIRE(t.size() == 1);
    CHECK(nodesOf(t, 0) == std::vector<std::uint32_t>{0, 1, 2});
}

TEST_CASE("small tasks of one level join in order until they hold the minimum; large ones stay apart") {
    // A source 0 fans out to 1..6, which a sink 7 gathers: 1..6 are one
    // level and reach none of each other.
    const auto deps = support::listOf({{}, {0}, {0}, {0}, {0}, {0}, {0}, {1, 2, 3, 4, 5, 6}});
    SECTION("small") {
        const std::vector<double> cost{1, 8, 8, 8, 8, 8, 8, 1};
        const auto t = primitives::tasksOf(deps, cost, 20.0);
        REQUIRE(t.size() == 4);
        CHECK(nodesOf(t, 1) == std::vector<std::uint32_t>{1, 2, 3});  // 24 >= 20 closes it
        CHECK(nodesOf(t, 2) == std::vector<std::uint32_t>{4, 5, 6});
        CHECK(t.waits == std::vector<std::uint32_t>{0, 1, 1, 2});
    }
    SECTION("large") {
        const std::vector<double> cost{1, 30, 30, 30, 30, 30, 30, 1};
        CHECK(primitives::tasksOf(deps, cost, 20.0).size() == 8);
    }
}

TEST_CASE("tasks of different levels do not join, so no task waits on itself") {
    // 0 -> 1 -> 3 and 0 -> 2 -> 3, with 1 also feeding 2: 2 is a level
    // below 1, and joining them would make a cycle through nothing.
    const auto deps = support::listOf({{}, {0}, {0, 1}, {1, 2}});
    const std::vector<double> cost{1, 1, 1, 1};
    std::mt19937 random(Catch::getSeed());
    const auto t = primitives::tasksOf(deps, cost, 1000.0);
    support::checkTasks(deps, t, random);
}

TEST_CASE("the cost table is sorted, names registered types, and has a default for the rest") {
    const auto costs = primitives::typeCosts();
    CHECK(std::ranges::is_sorted(costs, {}, &primitives::typeCost::type));
    const auto types = primitives::registeredTypes();
    for (const auto& c : costs) {
        INFO(c.type);
        CHECK((c.type == "sum" || std::ranges::find(types, c.type) != types.end()));
        CHECK(c.nanoseconds > primitives::defaultCost);
    }
    CHECK(primitives::costOf("ladder") == 57.0);
    CHECK(primitives::costOf("gain") == primitives::defaultCost);
}

TEST_CASE("for random dependencies, tasks keep every dependency and admit no early node") {
    std::mt19937 random(Catch::getSeed());  // --rng-seed repeats a run
    auto below = [&](std::size_t n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(random); };
    std::size_t tasks = 0;
    std::size_t nodes = 0;
    for (int graph = 0; graph < 300; ++graph) {
        const std::size_t n = 1 + below(80);
        const auto deps = support::randomDeps(n, random);
        std::vector<double> cost(n);
        for (auto& c : cost) c = static_cast<double>(below(30));
        const auto t = primitives::tasksOf(deps, cost, static_cast<double>(below(100)));
        INFO("graph " << graph);
        support::checkTasks(deps, t, random);
        tasks += t.size();
        nodes += n;
    }
    CHECK(tasks < nodes);  // something joined
}
