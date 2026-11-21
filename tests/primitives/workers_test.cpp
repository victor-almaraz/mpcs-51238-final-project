// The task queue and the worker pool (threads.md §6): every node of a
// block runs once, after every node it depends on, at every thread count.

#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <thread>
#include <vector>

#include <espresso/primitives/plan.h>
#include <espresso/primitives/realtime.h>
#include <espresso/protocol/jthread.h>

#include "../../src/primitives/tasks.h"
#include "../../src/primitives/workers.h"

using primitives::dependencyList;
using primitives::taskCounters;

namespace {

// Records the order nodes run in, from any thread.
class recorder final : public primitives::nodeRunner {
public:
    explicit recorder(std::size_t nodes) : stamp_(nodes) {}

    void run(std::uint32_t node) noexcept ESPRESSO_NONBLOCKING override {
        stamp_[node].store(clock_.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        runs_[node % runs_.size()].fetch_add(1, std::memory_order_relaxed);
    }

    // Clears the stamps for the next block.
    void reset() {
        for (auto& s : stamp_) s.store(0, std::memory_order_relaxed);
        for (auto& r : runs_) r.store(0, std::memory_order_relaxed);
    }
    // When node @p i ran, from 1; 0 when it did not.
    std::uint64_t stamp(std::size_t i) const { return stamp_[i].load(std::memory_order_relaxed); }
    // The runs of all nodes.
    std::uint64_t runs() const {
        std::uint64_t total = 0;
        for (const auto& r : runs_) total += r.load(std::memory_order_relaxed);
        return total;
    }

private:
    std::vector<std::atomic<std::uint64_t>> stamp_;
    std::array<std::atomic<std::uint64_t>, 8> runs_{};
    std::atomic<std::uint64_t> clock_{0};
};

// A random dependency list of @p n nodes, as tasks_test's.
dependencyList randomDeps(std::size_t n, std::mt19937& random) {
    auto below = [&](std::size_t k) { return std::uniform_int_distribution<std::size_t>(0, k - 1)(random); };
    dependencyList d;
    for (std::size_t j = 0; j < n; ++j) {
        std::vector<std::uint32_t> b;
        b.reserve(3);
        const std::size_t k = j == 0 || below(4) == 0 ? 0 : 1 + below(std::min<std::size_t>(j, 3));
        for (std::size_t e = 0; e < k; ++e) b.push_back(static_cast<std::uint32_t>(below(j)));
        std::ranges::sort(b);
        b.erase(std::ranges::unique(b).begin(), b.end());
        d.before.insert(d.before.end(), b.begin(), b.end());
        d.start.push_back(static_cast<std::uint32_t>(d.before.size()));
    }
    return d;
}

}  // namespace

TEST_CASE("the task queue gives back what it took, in order, and refuses past its capacity") {
    primitives::taskQueue q(4);
    CHECK(q.capacity() == 4);
    std::uint32_t t = 0;
    CHECK_FALSE(q.pop(t));
    for (std::uint32_t i = 0; i < 4; ++i) CHECK(q.push(i));
    CHECK_FALSE(q.push(9));
    for (std::uint32_t i = 0; i < 4; ++i) {
        REQUIRE(q.pop(t));
        CHECK(t == i);
    }
    CHECK_FALSE(q.pop(t));
    CHECK(q.push(5));  // the cells come round again
    REQUIRE(q.pop(t));
    CHECK(t == 5);
}

TEST_CASE("the task queue loses and repeats nothing with several pushing and popping threads") {
    constexpr std::uint32_t perThread = 20000;
    primitives::taskQueue q(1024);
    std::vector<std::atomic<std::uint32_t>> seen(std::size_t{4} * perThread);
    std::atomic<std::uint32_t> taken{0};
    {
        std::vector<espresso::jthread> threads;
        for (std::uint32_t p = 0; p < 4; ++p) {
            threads.emplace_back([&, p] {
                for (std::uint32_t i = 0; i < perThread; ++i) {
                    while (!q.push(p * perThread + i)) primitives::cpuRelax();
                }
            });
            threads.emplace_back([&] {
                std::uint32_t t = 0;
                while (taken.load() < 4 * perThread) {
                    if (q.pop(t)) {
                        seen[t].fetch_add(1);
                        taken.fetch_add(1);
                    }
                }
            });
        }
    }
    CHECK(std::ranges::all_of(seen, [](const auto& s) { return s.load() == 1; }));
}

TEST_CASE("the pool runs every node once a block, after the nodes it depends on, at every thread count") {
    std::mt19937 random(Catch::getSeed());  // --rng-seed repeats a run
    for (const int threads : {1, 2, 4, 8}) {
        INFO(threads << " threads");
        primitives::workerPool pool(threads, nullptr);
        CHECK(pool.threads() == threads);
        for (int graph = 0; graph < 40; ++graph) {
            const std::size_t n = 1 + std::uniform_int_distribution<std::size_t>(0, 199)(random);
            const auto deps = randomDeps(n, random);
            const std::vector<double> cost(n, 5000.0);  // small: many tasks
            const auto tasks = primitives::tasksOf(deps, cost);
            taskCounters counters{.waiting = std::vector<std::atomic<std::uint32_t>>(tasks.size())};
            recorder r(n);
            std::size_t early = 0;
            for (int block = 0; block < 20; ++block) {
                r.reset();
                pool.run(tasks, counters, r);
                for (std::size_t j = 0; j < n; ++j) {
                    for (const auto i : deps.of(j)) {
                        if (r.stamp(i) == 0 || r.stamp(i) >= r.stamp(j)) ++early;
                    }
                }
                REQUIRE(r.runs() == n);
            }
            INFO("graph " << graph);
            CHECK(early == 0);
        }
    }
}

TEST_CASE("workers that slept wake for the next block, and a pool stops with workers asleep") {
    primitives::workerPool pool(4, nullptr);
    std::mt19937 random(Catch::getSeed());
    const auto deps = randomDeps(50, random);
    const auto tasks = primitives::tasksOf(deps, std::vector<double>(50, 1e6));  // one task a chain
    taskCounters counters{.waiting = std::vector<std::atomic<std::uint32_t>>(tasks.size())};
    recorder r(50);
    for (int block = 0; block < 3; ++block) {
        // Longer than the workers spin, so they sleep between blocks.
        std::this_thread::sleep_for(primitives::workerPool::spinTime * 4);
        r.reset();
        pool.run(tasks, counters, r);
        CHECK(r.runs() == 50);
    }
}

TEST_CASE("the pool calls the start hook on each worker's own thread") {
    std::atomic<int> started{0};
    {
        const primitives::workerPool pool(3, [&] { started.fetch_add(1); });
        while (started.load() < 2) std::this_thread::yield();
    }
    CHECK(started.load() == 2);
}

TEST_CASE("the pool reports each thread's time running tasks, and the blocks the audio thread waited in") {
    primitives::workerPool pool(3, nullptr);
    std::mt19937 random(Catch::getSeed());
    const auto deps = randomDeps(120, random);
    const auto tasks = primitives::tasksOf(deps, std::vector<double>(120, 5000.0));
    taskCounters counters{.waiting = std::vector<std::atomic<std::uint32_t>>(tasks.size())};
    recorder r(120);
    for (int block = 0; block < 50; ++block) pool.run(tasks, counters, r);
    std::array<std::uint64_t, 3> busy{};
    pool.takeBusy(busy);
    CHECK(busy[0] > 0);  // the audio thread runs tasks too
    std::array<std::uint64_t, 3> again{};
    pool.takeBusy(again);
    CHECK(again == std::array<std::uint64_t, 3>{});  // taken, so zeroed
    CHECK(pool.waitedBlocks() <= 50);
}
