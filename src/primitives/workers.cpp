// The engine's worker threads and the scheduler of a block's tasks
// (threads.md §6, §7).

#include "workers.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>

#include <espresso/primitives/denormals.h>
#include <espresso/primitives/plan.h>
#include <espresso/primitives/realtime.h>

namespace primitives {

// --- taskQueue -------------------------------------------------------------

taskQueue::taskQueue(std::size_t capacity)
    : mask_(std::bit_ceil(std::max<std::size_t>(capacity, 2)) - 1), cells_(mask_ + 1) {
    for (std::size_t i = 0; i < cells_.size(); ++i) cells_[i].sequence.store(i, std::memory_order_relaxed);
}

bool taskQueue::push(std::uint32_t task) noexcept ESPRESSO_NONBLOCKING {
    std::size_t at = enqueue_.load(std::memory_order_relaxed);
    for (;;) {
        cell& c = cells_[at & mask_];
        const std::size_t sequence = c.sequence.load(std::memory_order_acquire);
        const auto lag = static_cast<std::ptrdiff_t>(sequence) - static_cast<std::ptrdiff_t>(at);
        if (lag == 0) {
            if (enqueue_.compare_exchange_weak(at, at + 1, std::memory_order_relaxed)) {
                c.task = task;
                c.sequence.store(at + 1, std::memory_order_release);
                return true;
            }
        } else if (lag < 0) {
            return false;  // full
        } else {
            at = enqueue_.load(std::memory_order_relaxed);
        }
    }
}

bool taskQueue::pop(std::uint32_t& task) noexcept ESPRESSO_NONBLOCKING {
    std::size_t at = dequeue_.load(std::memory_order_relaxed);
    for (;;) {
        cell& c = cells_[at & mask_];
        const std::size_t sequence = c.sequence.load(std::memory_order_acquire);
        const auto lag = static_cast<std::ptrdiff_t>(sequence) - static_cast<std::ptrdiff_t>(at + 1);
        if (lag == 0) {
            if (dequeue_.compare_exchange_weak(at, at + 1, std::memory_order_relaxed)) {
                task = c.task;
                c.sequence.store(at + mask_ + 1, std::memory_order_release);
                return true;
            }
        } else if (lag < 0) {
            return false;  // empty
        } else {
            at = dequeue_.load(std::memory_order_relaxed);
        }
    }
}

// --- workerPool --------------------------------------------------------------

namespace {

// Nanoseconds on the monotonic clock.
std::uint64_t nowNanoseconds() noexcept ESPRESSO_NONBLOCKING {
    ESPRESSO_ASSUME_NONBLOCKING_BEGIN  // reads the kernel's time page (vDSO, commpage), no system call
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
    ESPRESSO_ASSUME_NONBLOCKING_END
}

}  // namespace

workerPool::workerPool(int threads, std::function<void()> onStart, std::chrono::microseconds spin)
    : onStart_(std::move(onStart)), spin_(spin), busy_(static_cast<std::size_t>(std::max(threads, 1))) {
    const auto workers = busy_.size() - 1;
    workers_.reserve(workers);
    for (std::size_t i = 1; i <= workers; ++i) workers_.emplace_back([this, i] { work(i); });
}

void workerPool::takeBusy(std::span<std::uint64_t> out) noexcept ESPRESSO_NONBLOCKING {
    for (std::size_t i = 0; i < out.size() && i < busy_.size(); ++i) {
        out[i] = busy_[i].nanoseconds.exchange(0, std::memory_order_relaxed);
    }
}

workerPool::~workerPool() {
    stopping_.store(true, std::memory_order_seq_cst);
    generation_.fetch_add(1, std::memory_order_seq_cst);
    generation_.notify_all();
    workers_.clear();  // joins, before the members the workers use are destroyed
}

void workerPool::run(const taskList& tasks, taskCounters& counters,
                     nodeRunner& runner) noexcept ESPRESSO_NONBLOCKING {
    const auto count = static_cast<std::uint32_t>(tasks.size());
    if (count == 0) return;
    tasks_ = tasks;
    counters_ = counters;
    runner_ = runner;
    for (std::uint32_t t = 0; t < count; ++t)
        counters.waiting[t].store(tasks.waits[t], std::memory_order_relaxed);
    left_.store(count, std::memory_order_relaxed);
    // Each push publishes the stores above to the thread that pops it.
    for (std::uint32_t t = 0; t < count; ++t) {
        if (tasks.waits[t] == 0) queue_.push(t);  // never full: a block queues each task once
    }
    // A worker counts itself sleeping before it reads the generation, so
    // either it sees this block's or this thread sees it sleeping.
    generation_.fetch_add(1, std::memory_order_seq_cst);
    if (sleeping_.load(std::memory_order_seq_cst) > 0) {
        ESPRESSO_ASSUME_NONBLOCKING_BEGIN  // a futex or ulock wake: no lock, and only when a worker sleeps
                                           // (threads.md §6.5)
        generation_.notify_all();
        ESPRESSO_ASSUME_NONBLOCKING_END
    }
    // Run ready tasks while any are left; with none ready, wait for the
    // tasks the workers are running, at most the longest of them
    // (threads.md §6.4).
    bool waited = false;
    while (left_.load(std::memory_order_acquire) > 0) {
        if (!runOne(0)) {
            waited = true;
            cpuRelax();
        }
    }
    if (waited) waited_.fetch_add(1, std::memory_order_relaxed);
}

bool workerPool::runOne(std::size_t slot) noexcept ESPRESSO_NONBLOCKING {
    std::uint32_t t = 0;
    if (!queue_.pop(t)) return false;
    const auto begin = nowNanoseconds();
    const taskList& tasks = tasks_.get();
    taskCounters& counters = counters_.get();
    nodeRunner& runner = runner_.get();
    for (const auto node : tasks.nodesOf(t)) runner.run(node);
    for (const auto next : tasks.successorsOf(t)) {
        // The last task a successor waits for queues it; the read-modify-
        // write chain carries every waited-for task's writes to it.
        if (counters.waiting[next].fetch_sub(1, std::memory_order_acq_rel) == 1) queue_.push(next);
    }
    busy_[slot].nanoseconds.fetch_add(nowNanoseconds() - begin, std::memory_order_relaxed);
    // The task's last touch of the block: after it, run() may return.
    left_.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}

void workerPool::work(std::size_t slot) noexcept {
    const denormalGuard guard;
    if (onStart_) onStart_();
    std::uint32_t seen = generation_.load(std::memory_order_acquire);
    auto worked = std::chrono::steady_clock::now();
    while (!stopping_.load(std::memory_order_acquire)) {
        if (runOne(slot)) {
            worked = std::chrono::steady_clock::now();
            continue;
        }
        if (const auto g = generation_.load(std::memory_order_acquire); g != seen) {
            seen = g;  // a new block: look for its tasks
            worked = std::chrono::steady_clock::now();
            continue;
        }
        if (std::chrono::steady_clock::now() - worked < spin_) {
            cpuRelax();
            continue;
        }
        sleeping_.fetch_add(1, std::memory_order_seq_cst);
        if (generation_.load(std::memory_order_seq_cst) == seen)
            generation_.wait(seen, std::memory_order_seq_cst);
        sleeping_.fetch_sub(1, std::memory_order_seq_cst);
    }
}

}  // namespace primitives
