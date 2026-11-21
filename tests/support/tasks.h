// Checks a task list against the dependencies of its nodes (threads.md §5,
// §9.2).

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include <espresso/primitives/plan.h>

namespace support {

/// Checks that @p tasks covers every node of @p deps once, in order within
/// each task; that every dependency between tasks is an edge, counted in
/// the waiting task's waits; that the task graph is acyclic; that each
/// task's cost sums its nodes' @p cost; and that a schedule drawn with
/// @p random runs no node before one it depends on.
inline void checkTasks(const primitives::dependencyList& deps, const primitives::taskList& tasks,
                       std::span<const double> cost, std::mt19937& random) {
    const std::size_t n = deps.size();
    const std::size_t count = tasks.size();
    REQUIRE(tasks.successorStart.size() == count + 1);
    REQUIRE(tasks.waits.size() == count);
    REQUIRE(tasks.cost.size() == count);
    REQUIRE(tasks.nodes.size() == n);

    std::vector<std::optional<std::uint32_t>> taskOf(n);
    std::size_t misordered = 0;
    std::size_t miscosted = 0;
    for (std::uint32_t t = 0; t < count; ++t) {
        const auto nodes = tasks.nodesOf(t);
        CHECK_FALSE(nodes.empty());
        if (!std::ranges::is_sorted(nodes) || std::ranges::adjacent_find(nodes) != nodes.end()) ++misordered;
        double sum = 0.0;
        for (const auto x : nodes) {
            REQUIRE(x < n);
            REQUIRE_FALSE(taskOf[x]);
            taskOf[x] = t;
            sum += cost[x];
        }
        if (std::abs(sum - tasks.cost[t]) > 1e-6 * (1.0 + sum)) ++miscosted;
    }
    CHECK(misordered == 0);
    CHECK(miscosted == 0);
    REQUIRE(std::ranges::all_of(taskOf, [](const auto& t) { return t.has_value(); }));

    std::vector<std::uint32_t> waits(count, 0);
    std::size_t selfEdges = 0;
    for (std::uint32_t t = 0; t < count; ++t) {
        const auto next = tasks.successorsOf(t);
        CHECK(std::ranges::is_sorted(next));
        CHECK(std::ranges::adjacent_find(next) == next.end());
        for (const auto u : next) {
            REQUIRE(u < count);
            if (u == t) ++selfEdges;
            ++waits[u];
        }
    }
    CHECK(selfEdges == 0);
    CHECK(waits == tasks.waits);

    std::size_t missing = 0;  // dependencies between tasks without an edge
    for (std::size_t j = 0; j < n; ++j) {
        for (const auto i : deps.of(j)) {
            const auto from = taskOf[i].value_or(0);  // every node has a task, checked above
            const auto to = taskOf[j].value_or(0);
            if (from != to && !std::ranges::binary_search(tasks.successorsOf(from), to)) ++missing;
        }
    }
    CHECK(missing == 0);

    // A random schedule: any ready task next. It finishes every task only
    // if the task graph is acyclic.
    std::vector<std::uint32_t> left = tasks.waits;
    std::vector<std::uint32_t> ready;
    for (std::uint32_t t = 0; t < count; ++t) {
        if (left[t] == 0) ready.push_back(t);
    }
    std::vector<bool> ran(n, false);
    std::size_t finished = 0;
    std::size_t early = 0;  // nodes run before a dependency
    while (!ready.empty()) {
        const auto pick = std::uniform_int_distribution<std::size_t>(0, ready.size() - 1)(random);
        const auto t = ready[pick];
        ready.erase(ready.begin() + static_cast<std::ptrdiff_t>(pick));
        for (const auto x : tasks.nodesOf(t)) {
            for (const auto i : deps.of(x)) {
                if (!ran[i]) ++early;
            }
            ran[x] = true;
        }
        ++finished;
        for (const auto u : tasks.successorsOf(t)) {
            if (--left[u] == 0) ready.push_back(u);
        }
    }
    CHECK(finished == count);
    CHECK(early == 0);
}

}  // namespace support
