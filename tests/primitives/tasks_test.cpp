// Tasks: chains, joining small tasks of one level, and the property test
// of threads.md §9.2 on random dependencies.

#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <random>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/plan.h>

#include "../../src/primitives/costs.h"
#include "../../src/primitives/tasks.h"
#include "support/pipes.h"
#include "support/tasks.h"

using primitives::nodeInfo;
using primitives::taskList;
using support::control;

namespace {

std::vector<std::uint32_t> nodesOf(const taskList& t, std::size_t i) {
    const auto n = t.nodes.of(i);
    return {n.begin(), n.end()};
}

// A node that reads @p reads and writes @p write, costly enough to be a
// task of its own unless it joins a chain.
nodeInfo costly(std::vector<metadata::pipeRef> reads, metadata::pipeRef write) {
    nodeInfo n;
    n.reads = std::move(reads);
    n.writes = {write};
    n.cost = 1e6;
    return n;
}

// Whether the schedule of @p nodes, in order, is parallel on two threads.
bool parallel(const std::vector<nodeInfo>& nodes) {
    const std::vector<std::reference_wrapper<const nodeInfo>> order(nodes.begin(), nodes.end());
    return primitives::scheduleOf(order, {}, {}, {.threads = 2}).tasks.parallel;
}

// Every chain a task of its own, on @p threads threads, run in parallel
// when the estimated saving reaches @p saving.
primitives::scheduleOptions finest(int threads, double saving = 0.0) {
    return {.minimumTaskCost = 0.0, .threads = threads, .minimumParallelSaving = saving};
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
        const auto t = primitives::tasksOf(deps, cost, {.minimumTaskCost = 20.0});
        REQUIRE(t.size() == 4);
        CHECK(nodesOf(t, 1) == std::vector<std::uint32_t>{1, 2, 3});  // 24 >= 20 closes it
        CHECK(nodesOf(t, 2) == std::vector<std::uint32_t>{4, 5, 6});
        CHECK(t.waits == std::vector<std::uint32_t>{0, 1, 1, 2});
    }
    SECTION("large") {
        const std::vector<double> cost{1, 30, 30, 30, 30, 30, 30, 1};
        CHECK(primitives::tasksOf(deps, cost, {.minimumTaskCost = 20.0}).size() == 8);
    }
}

TEST_CASE("tasks of different levels do not join, so no task waits on itself") {
    // 0 -> 1 -> 3 and 0 -> 2 -> 3, with 1 also feeding 2: 2 is a level
    // below 1, and joining them would make a cycle through nothing.
    const auto deps = support::listOf({{}, {0}, {0, 1}, {1, 2}});
    const std::vector<double> cost{1, 1, 1, 1};
    std::mt19937 random(Catch::getSeed());
    const auto t = primitives::tasksOf(deps, cost, {.minimumTaskCost = 1000.0});
    support::checkTasks(deps, t, random);
}

TEST_CASE("a schedule is parallel only when some level of its task graph holds two tasks") {
    CHECK_FALSE(parallel({}));
    // 0 -> 1 -> 2: one task.
    CHECK_FALSE(parallel(
        {costly({}, control(0)), costly({control(0)}, control(1)), costly({control(1)}, control(2))}));
    // 0 -> 1 -> 2 and 0 -> 2: three tasks, one a level.
    CHECK_FALSE(parallel({costly({}, control(0)), costly({control(0)}, control(1)),
                          costly({control(0), control(1)}, control(2))}));
    // 0 -> 1 and 0 -> 2: 1 and 2 share a level.
    CHECK(parallel(
        {costly({}, control(0)), costly({control(0)}, control(1)), costly({control(0)}, control(2))}));
    CHECK(parallel({costly({}, control(0)), costly({}, control(1))}));
}

TEST_CASE("a task list's work and critical path are its tasks' costs, each dependency between tasks added") {
    // 0 -> 1 -> 4 and 0 -> 2 -> 3 -> 4: 2 and 3 are a chain, so the tasks
    // are {0}, {1}, {2, 3}, {4}, whose nodes cost 10, 20, 12, and 1 us. In
    // parallel each dependency on another task's node adds d: one for 1,
    // one for 2, and two for 4.
    const auto deps = support::listOf({{}, {0}, {0}, {2}, {1, 3}});
    const std::vector<double> cost{10'000, 20'000, 5'000, 7'000, 1'000};
    constexpr double d = primitives::dependencyCost;
    const auto t = primitives::tasksOf(deps, cost, finest(2));
    REQUIRE(t.size() == 4);
    CHECK(nodesOf(t, 2) == std::vector<std::uint32_t>{2, 3});
    CHECK(t.serialWork == 43'000.0);
    CHECK(t.work == 43'000.0 + 4 * d);
    CHECK(t.criticalPath == 31'000.0 + 3 * d);  // 0, 1, 4
    // The serial work less the path, which exceeds the work on 2 threads.
    const double saving = 12'000.0 - 3 * d;
    CHECK(t.savingAt(1) == 0.0);
    CHECK(t.savingAt(2) == saving);
    CHECK(t.savingAt(4) == saving);
    CHECK(t.parallel);
    CHECK(primitives::tasksOf(deps, cost, finest(2, saving)).parallel);
    CHECK_FALSE(primitives::tasksOf(deps, cost, finest(2, saving + 1.0)).parallel);
    // Four independent nodes, no dependency: the path is the costliest.
    const auto apart =
        primitives::tasksOf(support::listOf({{}, {}, {}, {}}), std::span(cost).first(4), finest(2));
    CHECK(apart.serialWork == 42'000.0);
    CHECK(apart.work == 42'000.0);
    CHECK(apart.criticalPath == 20'000.0);
    CHECK(apart.savingAt(2) == 21'000.0);
    CHECK(apart.savingAt(4) == 22'000.0);
    // Two nodes that depend on one: the path and the dependencies cost more
    // than plan order saves, which is no saving.
    const auto fan =
        primitives::tasksOf(support::listOf({{}, {0}, {0}}), std::vector<double>(3, d), finest(2));
    CHECK(fan.size() == 3);
    CHECK(fan.criticalPath == 3 * d);
    CHECK(fan.savingAt(2) == 0.0);
    CHECK(fan.parallel);  // at a least saving of 0
    CHECK(primitives::tasksOf({}, {}, finest(2)).work == 0.0);
}

TEST_CASE("a schedule is parallel only when its estimated saving on the engine's threads reaches the least") {
    const auto apart = support::listOf({{}, {}, {}, {}});
    SECTION("a chain of tasks never is") {
        // 0 -> 1 -> 2 and 0 -> 2: three tasks, one a level.
        const auto deps = support::listOf({{}, {0}, {0, 1}});
        const std::vector<double> cost(3, 1e9);
        for (const int threads : {2, 4, 64}) {
            const auto t = primitives::tasksOf(deps, cost, finest(threads));
            CHECK(t.size() == 3);
            CHECK_FALSE(t.parallel);
        }
    }
    SECTION("small independent tasks are not, on two threads") {
        // Four 1 us tasks save 2 us on two threads.
        const std::vector<double> cost(4, 1'000.0);
        const auto t = primitives::tasksOf(apart, cost, {.minimumTaskCost = 0.0, .threads = 2});
        CHECK(t.size() == 4);
        CHECK(t.savingAt(2) == 2'000.0);
        CHECK_FALSE(t.parallel);
        CHECK(primitives::tasksOf(apart, cost, finest(2)).parallel);  // forced
    }
    SECTION("large independent tasks are") {
        const std::vector<double> cost(4, primitives::defaultMinimumParallelSaving);
        for (const int threads : {2, 4}) {
            CHECK(primitives::tasksOf(apart, cost, {.minimumTaskCost = 0.0, .threads = threads}).parallel);
        }
    }
    SECTION("one thread never is") {
        const std::vector<double> cost(4, 1e9);
        CHECK_FALSE(primitives::tasksOf(apart, cost, finest(1)).parallel);
        CHECK_FALSE(primitives::tasksOf(apart, cost, finest(1, -1.0)).parallel);
    }
    SECTION("an infinite least saving makes every schedule serial") {
        const std::vector<double> cost(4, 1e9);
        CHECK_FALSE(
            primitives::tasksOf(apart, cost, finest(4, std::numeric_limits<double>::infinity())).parallel);
    }
}

TEST_CASE("a parallel schedule takes the fewest threads that save within the least saving of all of them") {
    constexpr int unbounded = std::numeric_limits<int>::max();
    const auto on = [](int threads) {
        return primitives::scheduleOptions{.minimumTaskCost = 0.0, .threads = threads};
    };
    SECTION("a path longer than the work on two threads takes two") {
        // The tasks of the work and path test: {0}, {1}, {2, 3}, {4}, whose
        // path of 31 us exceeds the work on two threads, 21.5 us, so every
        // count saves the same, 12 us less three dependencies.
        const auto deps = support::listOf({{}, {0}, {0}, {2}, {1, 3}});
        const std::vector<double> cost{10'000, 20'000, 5'000, 7'000, 1'000};
        for (const int threads : {4, 8}) {
            const auto t = primitives::tasksOf(deps, cost, on(threads));
            REQUIRE(t.parallel);
            CHECK(t.savingAt(2) == t.savingAt(threads));
            CHECK(t.threads == 2);
        }
    }
    const auto apart = [](std::size_t n) {
        return support::listOf(std::vector<std::vector<std::uint32_t>>(n));
    };
    SECTION("a fourth thread that saves less than the least saving takes no task") {
        // Four 10 us tasks: 20 us on two threads, 13.3 on three, 10 on four.
        const auto t = primitives::tasksOf(apart(4), std::vector<double>(4, 10'000.0), on(4));
        REQUIRE(t.parallel);
        CHECK(t.savingAt(4) - t.savingAt(3) < primitives::defaultMinimumParallelSaving);
        CHECK(t.savingAt(4) - t.savingAt(2) >= primitives::defaultMinimumParallelSaving);
        CHECK(t.threads == 3);
    }
    SECTION("a schedule that saves enough on every thread takes them all") {
        // Eight 10 us tasks: 40 us on two threads, 26.7 on three, 20 on four.
        const auto t = primitives::tasksOf(apart(8), std::vector<double>(8, 10'000.0), on(4));
        REQUIRE(t.parallel);
        CHECK(t.threads == unbounded);
    }
    SECTION("two threads, a least saving of 0, and a serial schedule are never bounded") {
        const auto deps = support::listOf({{}, {0}, {0}, {2}, {1, 3}});
        const std::vector<double> cost{10'000, 20'000, 5'000, 7'000, 1'000};
        CHECK(primitives::tasksOf(deps, cost, on(2)).threads == unbounded);
        CHECK(primitives::tasksOf(deps, cost, finest(4)).threads == unbounded);
        const auto serial =
            primitives::tasksOf(deps, cost, finest(4, std::numeric_limits<double>::infinity()));
        CHECK_FALSE(serial.parallel);
        CHECK(serial.threads == unbounded);
    }
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
        const auto t = primitives::tasksOf(deps, cost, {.minimumTaskCost = static_cast<double>(below(100))});
        INFO("graph " << graph);
        support::checkTasks(deps, t, random);
        CHECK(t.serialWork == std::accumulate(cost.begin(), cost.end(), 0.0));  // whole numbers: exact
        CHECK(t.work >= t.serialWork);
        CHECK(t.criticalPath <= t.work);
        tasks += t.size();
        nodes += n;
    }
    CHECK(tasks < nodes);  // something joined
}

TEST_CASE("a shuffled order keeps every dependency and repeats for one seed") {
    std::mt19937 random(Catch::getSeed());
    std::size_t moved = 0;     // orders that differ from the plan's
    std::size_t reseeded = 0;  // graphs whose orders differ by seed
    for (int graph = 0; graph < 300; ++graph) {
        const std::size_t n = 1 + std::uniform_int_distribution<std::size_t>(0, 79)(random);
        const auto deps = support::randomDeps(n, random);
        const auto seed = static_cast<std::uint64_t>(graph);
        const auto order = primitives::shuffledOrder(deps, seed);
        INFO("graph " << graph);
        REQUIRE(order.size() == n);
        std::vector<std::size_t> at(n, n);
        for (std::size_t k = 0; k < n; ++k) {
            REQUIRE(order[k] < n);
            REQUIRE(at[order[k]] == n);  // each node once
            at[order[k]] = k;
        }
        std::size_t early = 0;
        for (std::size_t j = 0; j < n; ++j) {
            for (const auto i : deps.of(j)) {
                if (at[i] >= at[j]) ++early;
            }
        }
        CHECK(early == 0);
        CHECK(primitives::shuffledOrder(deps, seed) == order);
        if (!std::ranges::is_sorted(order)) ++moved;
        if (primitives::shuffledOrder(deps, seed + 1000) != order) ++reseeded;
    }
    CHECK(moved > 200);
    CHECK(reseeded > 200);
}

TEST_CASE("a schedule takes its least task cost and its shuffle seed from its options") {
    // Four independent nodes of 1 us each: one task by default, four at a
    // least cost of 0.
    std::vector<nodeInfo> nodes;
    for (int i = 0; i < 4; ++i) {
        nodes.push_back(costly({}, control(i)));
        nodes.back().cost = 1000.0;
    }
    const std::vector<std::reference_wrapper<const nodeInfo>> order(nodes.begin(), nodes.end());
    const auto coarse = primitives::scheduleOf(order, {}, {});
    CHECK(coarse.tasks.size() == 1);
    CHECK_FALSE(coarse.tasks.parallel);
    CHECK(coarse.serialOrder == std::vector<std::uint32_t>{0, 1, 2, 3});  // plan order
    const auto fine = primitives::scheduleOf(order, {}, {}, finest(2));
    CHECK(fine.tasks.size() == 4);
    CHECK(fine.tasks.parallel);
    const auto shuffled = primitives::scheduleOf(order, {}, {}, {.shuffleSeed = 3});
    CHECK(shuffled.tasks.size() == 1);
    CHECK(shuffled.serialOrder == primitives::shuffledOrder(shuffled.dependencies, 3));
}
