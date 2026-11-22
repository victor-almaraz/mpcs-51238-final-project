// The task queue and the worker pool (threads.md §6): every node of a
// block runs once, after every node it depends on, at every thread count;
// and every thread that runs nodes holds one floating-point mode (§6.6).

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

#include <espresso/primitives/denormals.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/plan.h>
#include <espresso/primitives/primitive.h>
#include <espresso/primitives/realtime.h>
#include <espresso/protocol/jthread.h>

#include "../../src/primitives/tasks.h"
#include "../../src/primitives/workers.h"
#include "support/bits.h"
#include "support/graph.h"
#include "support/rounds.h"
#include "support/tasks.h"
#include "support/timing.h"

using primitives::workerPool;

namespace {

// Spins until @p flag is set or @p limit passes, as a node waits without
// blocking.
void waitFor(const std::atomic<bool>& flag, std::chrono::microseconds limit) noexcept ESPRESSO_NONBLOCKING {
    const auto until = primitives::steadyNow() + limit;
    while (!flag.load(std::memory_order_acquire) && primitives::steadyNow() < until) primitives::cpuRelax();
}

// Spins for @p work, as a node with real work runs.
void spinFor(std::chrono::microseconds work) noexcept ESPRESSO_NONBLOCKING {
    const auto until = primitives::steadyNow() + work;
    while (primitives::steadyNow() < until) primitives::cpuRelax();
}

// Records the order nodes run in, from any thread, and which nodes ran on
// a thread other than the one that made the recorder.
class recorder final : public primitives::nodeRunner {
public:
    explicit recorder(std::size_t nodes) : stamp_(nodes), away_(nodes) {}

    void run(std::uint32_t node) noexcept ESPRESSO_NONBLOCKING override {
        stamp_[node].store(clock_.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        runs_[node % runs_.size()].fetch_add(1, std::memory_order_relaxed);
        ESPRESSO_ASSUME_NONBLOCKING_BEGIN  // a test's runner: reads the thread's ID
        away_[node].store(std::this_thread::get_id() != maker_, std::memory_order_relaxed);
        ESPRESSO_ASSUME_NONBLOCKING_END
    }

    // Clears the stamps for the next block.
    void reset() {
        for (auto& s : stamp_) s.store(0, std::memory_order_relaxed);
        for (auto& r : runs_) r.store(0, std::memory_order_relaxed);
        for (auto& a : away_) a.store(false, std::memory_order_relaxed);
        clock_.store(0, std::memory_order_relaxed);
    }
    // When node @p i ran, from 1; 0 when it did not.
    std::uint64_t stamp(std::size_t i) const { return stamp_[i].load(std::memory_order_relaxed); }
    // The runs of all nodes.
    std::uint64_t runs() const {
        std::uint64_t total = 0;
        for (const auto& r : runs_) total += r.load(std::memory_order_relaxed);
        return total;
    }
    // The nodes that last ran on another thread than the recorder's maker.
    std::size_t away() const {
        return static_cast<std::size_t>(
            std::ranges::count_if(away_, [](const auto& a) { return a.load(std::memory_order_relaxed); }));
    }
    // The nodes that did not run in order, each the next stamp after the
    // node before it, from 1.
    std::size_t outOfOrder() const {
        std::size_t wrong = 0;
        for (std::size_t i = 0; i < stamp_.size(); ++i) {
            if (stamp(i) != i + 1) ++wrong;
        }
        return wrong;
    }

private:
    std::vector<std::atomic<std::uint64_t>> stamp_;
    std::vector<std::atomic<bool>> away_;
    std::array<std::atomic<std::uint64_t>, 8> runs_{};
    std::atomic<std::uint64_t> clock_{0};
    std::thread::id maker_ = std::this_thread::get_id();
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
            spinFor(std::chrono::milliseconds(2));
            return;
        }
        const std::size_t self = node - 1;
        const std::size_t other = 1 - self;
        arrived_[self].store(true, std::memory_order_release);
        waitFor(arrived_[other], std::chrono::seconds(1));
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

// Runs each node for a while, spinning, as a node with real work does.
class busyWork final : public primitives::nodeRunner {
public:
    explicit busyWork(std::chrono::microseconds work) : work_(work) {}

    void run(std::uint32_t /*node*/) noexcept ESPRESSO_NONBLOCKING override { spinFor(work_); }

private:
    std::chrono::microseconds work_;
};

// Holds the first task that a worker takes in a block until release(),
// for at most support::settleTimeout, and has the audio thread's first
// node wait up to 100 ms for that hold to start, so that a worker that
// wakes takes a task. Each node then runs with @p r, so a held node is
// stamped when its hold ends.
class holdBack final : public primitives::nodeRunner {
public:
    explicit holdBack(recorder& r) : r_(r) {}

    void run(std::uint32_t node) noexcept ESPRESSO_NONBLOCKING override {
        ESPRESSO_ASSUME_NONBLOCKING_BEGIN  // a test's runner: reads the thread's ID
        const bool onAudio = std::this_thread::get_id() == audio_;
        ESPRESSO_ASSUME_NONBLOCKING_END
        if (!onAudio) {
            if (bool first = false; holding_.compare_exchange_strong(first, true))
                waitFor(released_, support::settleTimeout);
        } else if (!led_.exchange(true, std::memory_order_relaxed)) {
            waitFor(holding_, std::chrono::milliseconds(100));
        }
        r_.run(node);
    }

    // Clears the flags for the next block.
    void reset() {
        holding_.store(false);
        released_.store(false);
        led_.store(false);
    }
    // Ends the hold.
    void release() { released_.store(true, std::memory_order_release); }
    // True once a worker holds a task.
    bool held() const { return holding_.load(); }
    // True once release() was called.
    bool released() const { return released_.load(); }

private:
    recorder& r_;
    std::thread::id audio_ = std::this_thread::get_id();  // the thread that runs blocks
    std::atomic<bool> holding_{false};
    std::atomic<bool> released_{false};
    std::atomic<bool> led_{false};  // the audio thread's first node has run
};

// Tasks that can run at once, made to run on the pool whatever they save:
// a least saving of 0, for more than one thread (taskList::parallel).
constexpr primitives::scheduleOptions pooled{.threads = 2, .minimumParallelSaving = 0.0};

// @p count tasks of one node each, node @p first + t for task t, that wait
// for no task and that no task waits for, marked parallel.
primitives::taskList independentTasks(std::size_t count, std::uint32_t first = 0) {
    primitives::taskList tasks;
    tasks.parallel = true;
    const auto n = static_cast<std::uint32_t>(count);
    for (std::uint32_t t = 0; t < n; ++t) {
        tasks.nodes.push_back(first + t);
        tasks.start.push_back(t + 1);
        tasks.successorStart.push_back(0);
        tasks.waits.push_back(0);
    }
    return tasks;
}

// A schedule of independentTasks(@p count).
primitives::nodeSchedule independentSchedule(std::size_t count) {
    return support::scheduleFor(independentTasks(count));
}

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
    // A queue that loses an element fails at the deadline instead of hanging.
    const auto deadline = std::chrono::steady_clock::now() + support::settleTimeout;
    const auto late = [&] { return std::chrono::steady_clock::now() >= deadline; };
    {
        std::vector<espresso::jthread> threads;
        for (std::uint32_t p = 0; p < 4; ++p) {
            threads.emplace_back([&, p] {
                for (std::uint32_t i = 0; i < perThread; ++i) {
                    while (!q.push(p * perThread + i) && !late()) primitives::cpuRelax();
                }
            });
            threads.emplace_back([&] {
                std::uint32_t t = 0;
                while (taken.load() < 4 * perThread && !late()) {
                    if (q.pop(t)) {
                        seen[t].fetch_add(1);
                        taken.fetch_add(1);
                    }
                }
            });
        }
    }
    CHECK(taken.load() == 4 * perThread);
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
            auto s = support::scheduleFor(primitives::tasksOf(deps, cost, pooled));
            recorder r(n);
            std::size_t early = 0;
            for (int block = 0; block < 20; ++block) {
                r.reset();
                pool.run(s, r);
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

TEST_CASE("workers that slept wake for the next block and a pool whose workers sleep stops at once") {
    std::mt19937 random(Catch::getSeed());
    const auto deps = support::randomDeps(50, random);
    // One task a chain.
    auto s = support::scheduleFor(primitives::tasksOf(deps, std::vector<double>(50, 1e6), pooled));
    recorder r(50);
    auto pool = std::make_unique<primitives::workerPool>(4, nullptr);
    // Longer than the workers spin, so they sleep between blocks and before
    // the pool stops.
    const auto pause = pool->spin() * 4;
    for (int block = 0; block < 3; ++block) {
        std::this_thread::sleep_for(pause);
        r.reset();
        pool->run(s, r);
        CHECK(r.runs() == 50);
    }
    std::this_thread::sleep_for(pause);
    const auto start = std::chrono::steady_clock::now();
    pool.reset();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
}

TEST_CASE("a block's first tasks run at once on two threads whether the worker spins or sleeps") {
    // Nodes 1 and 2 of a rendezvous, each a task that waits for none: they
    // meet only when the start of the block brings the worker to one.
    auto s = support::scheduleFor(independentTasks(2, 1));
    for (const auto spin : {primitives::defaultWorkerSpin, std::chrono::microseconds{0}}) {
        INFO("spin " << spin.count() << " us");
        workerPool pool(2, nullptr, spin);
        rendezvous r(std::this_thread::get_id());
        for (int block = 0; block < 5; ++block) {
            // With no spin, the worker sleeps as soon as it finds no task,
            // so after a pause it sleeps; otherwise it spins from the last
            // block.
            if (spin == std::chrono::microseconds{0})
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            r.reset();
            pool.run(s, r);
            INFO("block " << block);
            CHECK(r.met(1));
            CHECK(r.met(2));
            if (!r.met(1) || !r.met(2)) break;  // each miss waits out the deadline
        }
    }
}

TEST_CASE("tasks that a task queues while a worker sleeps wake the worker") {
    // Task 0, then tasks 1 and 2, which wait for it and must meet.
    const auto deps = support::listOf({{}, {0}, {0}});
    auto s = support::scheduleFor(primitives::tasksOf(deps, std::vector<double>(3, 1e6), pooled));
    REQUIRE(s.tasks.parallel);
    REQUIRE(s.tasks.size() == 3);
    // One worker, which sleeps as soon as it finds no task: while the
    // audio thread runs task 0, it sleeps.
    primitives::workerPool pool(2, nullptr, std::chrono::microseconds{0});
    rendezvous r(std::this_thread::get_id());
    int checked = 0;
    for (int block = 0; block < 20; ++block) {
        r.reset();
        pool.run(s, r);
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

TEST_CASE("every thread of a pool reports time running tasks over blocks of real work") {
    // Eight tasks of 20 us a block, which workers awake from the last
    // block share with the audio thread.
    auto s = independentSchedule(8);
    busyWork r(std::chrono::microseconds(20));
    for (const int threads : {2, 4}) {
        INFO(threads << " threads");
        workerPool pool(threads, nullptr);
        const auto slots = static_cast<std::size_t>(threads);
        std::vector<std::uint64_t> busy(slots);
        std::vector<std::uint64_t> total(slots);
        // Until every thread has run a task, within a deadline: a thread
        // the machine gives to other work can miss blocks.
        const auto idle = [&] { return std::ranges::any_of(total, [](std::uint64_t t) { return t == 0; }); };
        const auto deadline = std::chrono::steady_clock::now() + support::settleTimeout;
        while (idle() && std::chrono::steady_clock::now() < deadline) {
            pool.run(s, r);
            pool.takeBusy(busy);
            for (std::size_t i = 0; i < slots; ++i) total[i] += busy[i];
        }
        CHECK_FALSE(idle());
        pool.takeBusy(busy);
        CHECK(std::ranges::all_of(busy, [](std::uint64_t b) { return b == 0; }));  // taken, so zeroed
    }
}

TEST_CASE("a task a worker holds back keeps the block open until it finishes") {
    // Six tasks; a worker holds the first it takes until a helper thread
    // releases it, 2 ms after every other node has run. The audio thread
    // runs the rest, then waits for the worker: the late worker of
    // threads.md §6.3, and the barrier at the end of the block. A block
    // counts as waited only when the audio thread found no task while the
    // held one was out; one the system kept from running until the release
    // never waited, so every held block checks the invariants and some held
    // block must have been counted.
    constexpr std::size_t n = 6;
    auto s = independentSchedule(n);
    for (const int threads : {2, 4}) {
        for (const auto spin : {primitives::defaultWorkerSpin, std::chrono::microseconds{0}}) {
            INFO(threads << " threads, spin " << spin.count() << " us");
            recorder r(n);
            holdBack h(r);
            workerPool pool(threads, nullptr, spin);
            int heldBlocks = 0;
            int countedBlocks = 0;
            for (int block = 0; block < 50 && countedBlocks == 0; ++block) {
                INFO("block " << block);
                r.reset();
                h.reset();
                const auto waited = pool.waitedBlocks();
                bool releasedFirst = false;
                std::uint64_t runs = 0;
                {
                    const espresso::jthread releaser([&](const espresso::stop_token& stop) {
                        support::waitUntil([&] { return h.held() || stop.stop_requested(); });
                        if (!h.held()) return;  // no worker woke within the audio thread's wait
                        support::waitUntil([&] { return r.runs() == n - 1; });  // the held node alone is left
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        h.release();
                    });
                    pool.run(s, h);
                    // Read as run() returns, before the helper is joined.
                    releasedFirst = h.released();
                    runs = r.runs();
                }
                CHECK(runs == n);
                if (!h.held()) continue;
                ++heldBlocks;
                CHECK(releasedFirst);  // run() returned only after the held task finished
                CHECK(pool.waitedBlocks() <= waited + 1);  // one block counts once
                if (pool.waitedBlocks() == waited + 1) ++countedBlocks;
            }
            CHECK(heldBlocks > 0);
            CHECK(countedBlocks > 0);
        }
    }
}

TEST_CASE("a schedule runs as tasks on the pool only when it is parallel and fits") {
    workerPool pool(3, nullptr);
    std::array<std::uint64_t, 3> busy{};
    // Exactly maxTasks tasks fill the queue and run on the pool: nodes 1 and
    // 2 meet, so a worker ran one of them.
    {
        auto s = independentSchedule(workerPool::maxTasks);
        recorder r(workerPool::maxTasks);
        rendezvous m(std::this_thread::get_id());
        // Records every node, and has nodes 1 and 2 meet as well.
        class meeting final : public primitives::nodeRunner {
        public:
            meeting(recorder& r, rendezvous& m) : r_(r), m_(m) {}

            void run(std::uint32_t node) noexcept ESPRESSO_NONBLOCKING override {
                if (node == 1 || node == 2) m_.run(node);
                r_.run(node);
            }

        private:
            recorder& r_;
            rendezvous& m_;
        } both(r, m);
        pool.run(s, both);
        CHECK(r.runs() == workerPool::maxTasks);
        CHECK(m.met(1));
        CHECK(m.met(2));
        pool.takeBusy(busy);
        CHECK(busy[1] + busy[2] > 0);
    }
    // One more runs in order on this thread, and no worker runs a task.
    {
        constexpr std::size_t n = workerPool::maxTasks + 1;
        auto s = independentSchedule(n);
        recorder r(n);
        pool.run(s, r);
        CHECK(r.runs() == n);
        CHECK(r.outOfOrder() == 0);
        CHECK(r.away() == 0);
        pool.takeBusy(busy);
        CHECK(busy[0] > 0);
        CHECK(busy[1] == 0);
        CHECK(busy[2] == 0);
    }
    // So does a schedule that is not parallel, and any schedule without a
    // pool, which is not timed.
    {
        auto s = independentSchedule(8);
        s.tasks.parallel = false;
        recorder r(8);
        pool.run(s, r);
        CHECK(r.outOfOrder() == 0);
        CHECK(r.away() == 0);
        pool.takeBusy(busy);
        CHECK(busy[0] > 0);
        CHECK(busy[1] + busy[2] == 0);
        s.tasks.parallel = true;
        r.reset();
        primitives::runSerially(s, r);
        CHECK(r.outOfOrder() == 0);
        CHECK(r.away() == 0);
        pool.takeBusy(busy);
        CHECK(busy == std::array<std::uint64_t, 3>{});
    }
}

TEST_CASE("bench: the pool's cost per task", "[.bench]") {
    // A thousand independent tasks of one node that does nothing: a block
    // costs the scheduler's bookkeeping alone (threads.md §5.2, §11 step 7).
    constexpr std::size_t n = 1000;
    auto s = independentSchedule(n);
    class nothing final : public primitives::nodeRunner {
    public:
        void run(std::uint32_t /*node*/) noexcept ESPRESSO_NONBLOCKING override {}
    } runner;
    // Each count a round, in an order rotated from round to round, after
    // a block that wakes the workers (support::inRotation).
    constexpr std::array counts{1, 2, 4, 8, 16};
    constexpr std::size_t rounds = 5;
    std::array<std::vector<double>, counts.size()> costs;
    support::inRotation(counts.size(), rounds, [&](std::size_t round, std::size_t c) {
        primitives::workerPool pool(counts[c], nullptr);
        pool.run(s, runner);
        constexpr int blocks = 2000;
        const auto start = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b) pool.run(s, runner);
        const std::chrono::duration<double, std::nano> took = std::chrono::steady_clock::now() - start;
        costs[c].push_back(took.count() / (blocks * n));
        std::cout << "bench task-cost round " << round + 1 << " threads " << counts[c] << " "
                  << costs[c].back() << " ns a task\n";
    });
    for (std::size_t c = 0; c < counts.size(); ++c) {
        std::cout << "bench task-cost summary threads " << counts[c] << ", " << rounds
                  << " rounds: median (range) " << support::spreadOf(costs[c]) << " ns a task\n";
    }
    CHECK(true);
}

namespace {

// A floating-point mode unlike a node thread's: round toward zero,
// denormals kept, every exception masked.
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
constexpr std::uint64_t roundTowardZero = 0x7F80;
#elif defined(__aarch64__)
constexpr std::uint64_t roundTowardZero = std::uint64_t{3} << 22U;
#else
constexpr std::uint64_t roundTowardZero = 0;
#endif

// A node that records, on the thread that runs it, that thread and its
// floating-point mode, then works for 50 us, so that the workers
// take some of a block's nodes. Read between blocks: the end of each
// block orders the writes before the reads.
class floatProbe final : public primitives::primitive {
public:
    floatProbe() : primitive("probe", "lfo") {}

    void process(const primitives::blockContext& /*ctx*/) noexcept ESPRESSO_NONBLOCKING override {
        mode_ = primitives::floatMode();
        ESPRESSO_ASSUME_NONBLOCKING_BEGIN  // a test's node: reads the thread's ID
        thread_ = std::this_thread::get_id();
        ESPRESSO_ASSUME_NONBLOCKING_END
        spinFor(std::chrono::microseconds(50));
    }

    std::uint64_t mode() const { return mode_; }
    std::thread::id thread() const { return thread_; }

private:
    std::uint64_t mode_ = 0;
    std::thread::id thread_;
};

}  // namespace

TEST_CASE(
    "every thread that runs a node holds the node threads' floating-point mode whatever its creator's") {
    // The engine's creator rounds toward zero and keeps denormals, and
    // enters each block with that mode. On Linux its workers start with
    // it too, since a new thread inherits its creator's floating-point
    // state; on macOS they start with the system's default.
    const primitives::floatControlScope inherited(roundTowardZero);
    support::renderer r({.audio = {.sampleRate = 48000.0, .blockSize = 64},
                         .pipes = {.control = 8},
                         .threads = 4,
                         .schedule = support::finest},
                        1);
    // Eight independent nodes, each a task of its own, each a probe.
    support::graph g;
    for (int i = 0; i < 8; ++i) {
        g.pipe(support::control(i));
        g.node("lfo" + std::to_string(i), "lfo").outputs["out"] = {support::control(i)};
    }
    const auto p = r.engine.build(g.spec, 1);
    REQUIRE(p->schedule.tasks.parallel);
    std::vector<std::shared_ptr<floatProbe>> probes;
    for (auto& node : p->nodes) {
        probes.push_back(std::make_shared<floatProbe>());
        node = probes.back();
    }
    r.engine.play(p, 0.0, primitives::quantization::none);
    const auto audio = std::this_thread::get_id();
    std::set<std::thread::id> threads;
    std::size_t wrong = 0;
    // Until the audio thread and a worker have each run a node, within a
    // deadline: a worker the machine gives to other work can miss blocks.
    const auto deadline = std::chrono::steady_clock::now() + support::settleTimeout;
    while ((threads.size() < 2 || !threads.contains(audio)) && std::chrono::steady_clock::now() < deadline) {
        r.run(64);
        for (const auto& probe : probes) {
            if (probe->mode() != primitives::nodeFloatControl) ++wrong;
            threads.insert(probe->thread());
        }
    }
    CHECK(threads.contains(audio));
    CHECK(threads.size() >= 2);
    CHECK(wrong == 0);
    CHECK(primitives::floatMode() == roundTowardZero);  // each block restored the caller's mode
}

TEST_CASE("a comb's tail decaying through the denormal range renders the same bits on 1 and 4 threads") {
    // An impulse into four combs of 1 to 1.3 ms at feedback 0.9, each to a
    // device output: each tail falls below the smallest normal float after
    // 0.8 to 1.0 s, where flush-to-zero ends it.
    support::graph g;
    g.pipe(support::audio(0));
    g.node("in", "audioIn").outputs["out"] = {support::audio(0)};
    for (int i = 0; i < 4; ++i) {
        g.pipe(support::audio(i + 1));
        auto& comb = g.node("comb" + std::to_string(i), "comb");
        comb.params = {{"time", 0.001 + (0.0001 * i)}, {"feedback", 0.9}};
        comb.inputs["in"] = {support::audio(0)};
        comb.outputs["out"] = {support::audio(i + 1)};
        auto& out = g.node("out" + std::to_string(i), "audioOut");
        out.params = {{"channel", static_cast<double>(i)}};
        out.inputs["in"] = {support::audio(i + 1)};
    }
    constexpr int block = 256;
    constexpr int blocks = 300;  // 1.6 s: the decay, then the half second checked
    auto render = [&](int threads) {
        support::renderer r({.audio = {.sampleRate = 48000.0, .blockSize = block},
                             .pipes = {.audio = 5},
                             .threads = threads,
                             .schedule = support::finest},
                            4);
        r.play(g.spec);
        std::vector<std::vector<float>> impulse(1, std::vector<float>(block, 0.0f));
        impulse[0][0] = 1.0f;
        r.run(block, impulse);
        for (int b = 1; b < blocks; ++b) r.run(block);
        return std::vector<std::vector<float>>{r.out(0), r.out(1), r.out(2), r.out(3)};
    };
    const auto one = render(1);
    for (const auto& channel : one) {
        CHECK(support::peak(channel, 0, 4800) > 0.1);
        CHECK(support::peak(channel, channel.size() - 24000) == 0.0);  // flushed by the last half second
    }
    CHECK(support::bitDifference(render(4), one) == "");
}
