// The task queue and the worker pool (threads.md §6): every node of a
// block runs once, after every node it depends on, at every thread count.

#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <ratio>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>

#include <espresso/primitives/plan.h>
#include <espresso/primitives/realtime.h>
#include <espresso/protocol/jthread.h>

#include "../../src/primitives/tasks.h"
#include "../../src/primitives/workers.h"
#include "support/tasks.h"

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

// Runs node 0 for about 2 ms, noting whether it ran on the thread given,
// and has nodes 1 and 2 wait up to a second for each other: they meet only
// when they run at once, on two threads.
class rendezvous final : public primitives::nodeRunner {
public:
    explicit rendezvous(std::thread::id caller) : caller_(caller) {}

    void run(std::uint32_t node) noexcept ESPRESSO_NONBLOCKING override {
        if (node == 0) {
            ESPRESSO_ASSUME_NONBLOCKING_BEGIN  // a test's runner: reads the thread's ID
            onCaller_.store(std::this_thread::get_id() == caller_, std::memory_order_relaxed);
            ESPRESSO_ASSUME_NONBLOCKING_END
            const auto until = primitives::steadyNow() + std::chrono::milliseconds(2);
            while (primitives::steadyNow() < until) primitives::cpuRelax();
            return;
        }
        const std::size_t self = node - 1;
        const std::size_t other = 1 - self;
        arrived_[self].store(true, std::memory_order_release);
        const auto until = primitives::steadyNow() + std::chrono::seconds(1);
        while (!arrived_[other].load(std::memory_order_acquire) && primitives::steadyNow() < until)
            primitives::cpuRelax();
        met_[self].store(arrived_[other].load(std::memory_order_acquire), std::memory_order_relaxed);
    }

    // Clears the flags for the next block.
    void reset() {
        onCaller_.store(false);
        for (auto& a : arrived_) a.store(false);
        for (auto& m : met_) m.store(false);
    }
    // True when node 0 ran on the thread given.
    bool firstOnCaller() const { return onCaller_.load(); }
    // True when node @p i (1 or 2) saw the other arrive.
    bool met(std::size_t i) const { return met_[i - 1].load(); }

private:
    std::thread::id caller_;
    std::atomic<bool> onCaller_{false};
    std::array<std::atomic<bool>, 2> arrived_{};
    std::array<std::atomic<bool>, 2> met_{};
};

// The calling thread's name, or empty when it cannot be read. POSIX
// threads return it into a buffer by pointer; Linux names have at most 15
// characters and macOS names 63.
std::string threadName() {
    std::array<char, 64> name{};
    if (::pthread_getname_np(::pthread_self(), name.data(), name.size()) != 0) return {};
    return name.data();
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
            const auto deps = support::randomDeps(n, random);
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
    const auto deps = support::randomDeps(50, random);
    const auto tasks = primitives::tasksOf(deps, std::vector<double>(50, 1e6));  // one task a chain
    taskCounters counters{.waiting = std::vector<std::atomic<std::uint32_t>>(tasks.size())};
    recorder r(50);
    for (int block = 0; block < 3; ++block) {
        // Longer than the workers spin, so they sleep between blocks.
        std::this_thread::sleep_for(pool.spin() * 4);
        r.reset();
        pool.run(tasks, counters, r);
        CHECK(r.runs() == 50);
    }
}

TEST_CASE("tasks that a task queues while a worker sleeps wake the worker") {
    // Task 0, then tasks 1 and 2, which wait for it and must meet.
    const auto deps = support::listOf({{}, {0}, {0}});
    const auto tasks = primitives::tasksOf(deps, std::vector<double>(3, 1e6));
    REQUIRE(tasks.size() == 3);
    taskCounters counters{.waiting = std::vector<std::atomic<std::uint32_t>>(tasks.size())};
    // One worker, which sleeps as soon as it finds no task: while the
    // audio thread runs task 0, it sleeps.
    primitives::workerPool pool(2, nullptr, std::chrono::microseconds{0});
    rendezvous r(std::this_thread::get_id());
    int checked = 0;
    for (int block = 0; block < 20; ++block) {
        r.reset();
        pool.run(tasks, counters, r);
        // When the worker ran task 0, it was awake to queue 1 and 2.
        if (!r.firstOnCaller()) continue;
        ++checked;
        INFO("block " << block);
        CHECK(r.met(1));
        CHECK(r.met(2));
        if (!r.met(1) || !r.met(2)) break;  // each miss waits out the deadline
    }
    CHECK(checked > 0);
}

TEST_CASE("the pool calls the start hook on each worker's own thread, named for the worker") {
    std::mutex mutex;
    std::vector<std::thread::id> ids;
    std::vector<std::string> names;
    int workers = 0;
    {
        const primitives::workerPool pool(3, [&] {
            const std::string name = threadName();
            const std::scoped_lock lock(mutex);
            ids.push_back(std::this_thread::get_id());
            names.push_back(name);
        });
        workers = pool.threads() - 1;
    }  // the destructor waits for every hook
    CHECK(workers == 2);
    REQUIRE(ids.size() == static_cast<std::size_t>(workers));
    CHECK(std::set<std::thread::id>(ids.begin(), ids.end()).size() == ids.size());
    CHECK(std::ranges::find(ids, std::this_thread::get_id()) == ids.end());
    std::ranges::sort(names);
    CHECK(names == std::vector<std::string>{"espresso-w1", "espresso-w2"});
}

TEST_CASE("a pool whose workers sleep stops at once") {
    std::mt19937 random(Catch::getSeed());
    const auto deps = support::randomDeps(50, random);
    const auto tasks = primitives::tasksOf(deps, std::vector<double>(50, 1e6));
    taskCounters counters{.waiting = std::vector<std::atomic<std::uint32_t>>(tasks.size())};
    recorder r(50);
    auto pool = std::make_unique<primitives::workerPool>(3, nullptr, std::chrono::microseconds{0});
    pool->run(tasks, counters, r);
    REQUIRE(r.runs() == 50);
    // With no spin, a worker sleeps as soon as it finds no task.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto start = std::chrono::steady_clock::now();
    pool.reset();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
}

TEST_CASE("the pool reports each thread's time running tasks, and the blocks the audio thread waited in") {
    primitives::workerPool pool(3, nullptr);
    std::mt19937 random(Catch::getSeed());
    const auto deps = support::randomDeps(120, random);
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

TEST_CASE("bench: the pool's cost per task", "[.bench]") {
    // A thousand independent tasks of one node that does nothing: a block
    // costs the scheduler's bookkeeping alone (threads.md §5.2, §11 step 7).
    constexpr std::size_t n = 1000;
    dependencyList deps;
    for (std::size_t j = 0; j < n; ++j) deps.start.push_back(0);
    const auto tasks = primitives::tasksOf(deps, std::vector<double>(n, 1.0), 0.0);
    REQUIRE(tasks.size() == n);
    taskCounters counters{.waiting = std::vector<std::atomic<std::uint32_t>>(n)};
    class nothing final : public primitives::nodeRunner {
    public:
        void run(std::uint32_t /*node*/) noexcept ESPRESSO_NONBLOCKING override {}
    } runner;
    for (const int threads : {1, 2, 4}) {
        primitives::workerPool pool(threads, nullptr);
        constexpr int blocks = 2000;
        const auto start = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b) pool.run(tasks, counters, runner);
        const std::chrono::duration<double, std::nano> took = std::chrono::steady_clock::now() - start;
        std::cout << "bench task-cost threads " << threads << " " << took.count() / (blocks * n)
                  << " ns a task\n";
    }
    CHECK(true);
}
