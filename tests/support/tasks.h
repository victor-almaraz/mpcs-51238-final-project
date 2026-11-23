// Checks a task list against the dependencies of its nodes (threads.md §5,
// §9.2), and makes schedules of task lists for the pool.

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include <espresso/primitives/plan.h>

namespace support {

/// A dependency list from each node's dependencies, as given.
inline primitives::dependencyList listOf(const std::vector<std::vector<std::uint32_t>>& before) {
    primitives::dependencyList d;
    for (const auto& b : before) {
        d.items.insert(d.items.end(), b.begin(), b.end());
        d.start.push_back(static_cast<std::uint32_t>(d.items.size()));
    }
    return d;
}

/// A random dependency list of @p n nodes drawn with @p random: mostly
/// chains and fans, as instruments make, with some joins.
inline primitives::dependencyList randomDeps(std::size_t n, std::mt19937& random) {
    auto below = [&](std::size_t k) { return std::uniform_int_distribution<std::size_t>(0, k - 1)(random); };
    std::vector<std::vector<std::uint32_t>> before(n);
    for (std::size_t j = 0; j < n; ++j) {
        const std::size_t k = j == 0 || below(4) == 0 ? 0 : 1 + below(std::min<std::size_t>(j, 3));
        for (std::size_t e = 0; e < k; ++e) before[j].push_back(static_cast<std::uint32_t>(below(j)));
        std::ranges::sort(before[j]);
        before[j].erase(std::ranges::unique(before[j]).begin(), before[j].end());
    }
    return listOf(before);
}

/// A schedule of @p tasks alone, as the pool runs it: a counter for each
/// task, and the tasks' nodes in their own order as its serial order.
inline primitives::nodeSchedule scheduleFor(primitives::taskList tasks) {
    primitives::nodeSchedule s;
    s.counters.waiting = std::vector<std::atomic<std::uint32_t>>(tasks.size());
    s.serialOrder = tasks.nodes.items;
    std::ranges::sort(s.serialOrder);
    s.tasks = std::move(tasks);
    return s;
}

/// Checks that @p tasks covers every node of @p deps once, in order within
/// each task; that every dependency between tasks is an edge, counted in
/// the waiting task's waits; that the task graph is acyclic; and that a
/// schedule drawn with @p random runs no node before one it depends on.
inline void checkTasks(const primitives::dependencyList& deps, const primitives::taskList& tasks,
                       std::mt19937& random) {
    const std::size_t n = deps.size();
    const std::size_t count = tasks.size();
    REQUIRE(tasks.successors.size() == count);
    REQUIRE(tasks.waits.size() == count);
    REQUIRE(tasks.nodes.items.size() == n);

    std::vector<std::optional<std::uint32_t>> taskOf(n);
    std::size_t misordered = 0;
    for (std::uint32_t t = 0; t < count; ++t) {
        const auto nodes = tasks.nodes.of(t);
        CHECK_FALSE(nodes.empty());
        if (!std::ranges::is_sorted(nodes) || std::ranges::adjacent_find(nodes) != nodes.end()) ++misordered;
        for (const auto x : nodes) {
            REQUIRE(x < n);
            REQUIRE_FALSE(taskOf[x]);
            taskOf[x] = t;
        }
    }
    CHECK(misordered == 0);
    REQUIRE(std::ranges::all_of(taskOf, [](const auto& t) { return t.has_value(); }));

    std::vector<std::uint32_t> waits(count, 0);
    std::size_t selfEdges = 0;
    for (std::uint32_t t = 0; t < count; ++t) {
        const auto next = tasks.successors.of(t);
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
            if (from != to && !std::ranges::binary_search(tasks.successors.of(from), to)) ++missing;
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
        for (const auto x : tasks.nodes.of(t)) {
            for (const auto i : deps.of(x)) {
                if (!ran[i]) ++early;
            }
            ran[x] = true;
        }
        ++finished;
        for (const auto u : tasks.successors.of(t)) {
            if (--left[u] == 0) ready.push_back(u);
        }
    }
    CHECK(finished == count);
    CHECK(early == 0);
}

}  // namespace support
