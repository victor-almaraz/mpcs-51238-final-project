// The engine's worker threads and the scheduler of a block's tasks
// (threads.md §6, §7).

#include "workers.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <pthread.h>

#include <espresso/primitives/denormals.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/plan.h>
#include <espresso/primitives/realtime.h>
#include <espresso/protocol/jthread.h>

namespace primitives {

namespace {

// Pushes @p task to @p queue, which a block cannot fill: a block on the
// pool has at most maxTasks tasks, the queue's cells (run() checks it),
// queues each task once, and starts with the queue empty.
void pushTask(taskQueue& queue, std::uint32_t task) noexcept ESPRESSO_NONBLOCKING {
    [[maybe_unused]] const bool pushed = queue.push(task);
    ESPRESSO_ASSUME_NONBLOCKING_BEGIN  // assert only aborts, on a broken bound
    assert(pushed);
    ESPRESSO_ASSUME_NONBLOCKING_END
}

// Names the calling thread espresso-w<slot>, as debuggers, profilers, and
// top -H show it. The name has at most 12 characters, within Linux's 15,
// so naming cannot fail.
void nameWorker([[maybe_unused]] std::size_t slot) noexcept {
#if defined(__APPLE__) || defined(__linux__)
    const std::string name = "espresso-w" + std::to_string(slot);
#if defined(__APPLE__)
    [[maybe_unused]] const int named = ::pthread_setname_np(name.c_str());
#else
    [[maybe_unused]] const int named = ::pthread_setname_np(::pthread_self(), name.c_str());
#endif
    assert(named == 0);
#endif
}

// The workers of a pool of @p threads threads, from 1 to maxThreads.
std::size_t workersFor(int threads) {
    if (threads > maxThreads)
        throw std::invalid_argument("workerPool: at most " + std::to_string(maxThreads) + " threads");
    return static_cast<std::size_t>(std::max(threads, 1)) - 1;
}

}  // namespace

void runSerially(const nodeSchedule& schedule, nodeRunner& runner) noexcept ESPRESSO_NONBLOCKING {
    runner.runAll(schedule.serialOrder);
}

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

// --- sleepSemaphore ----------------------------------------------------------

#if defined(__APPLE__)
sleepSemaphore::sleepSemaphore() : semaphore_(dispatch_semaphore_create(0)) {
    if (!semaphore_)
        throw std::system_error(std::make_error_code(std::errc::not_enough_memory),
                                "cannot create the workers' semaphore");
}

sleepSemaphore::~sleepSemaphore() = default;

void sleepSemaphore::acquire() noexcept { dispatch_semaphore_wait(semaphore_.get(), DISPATCH_TIME_FOREVER); }

void sleepSemaphore::release(std::uint32_t tokens) noexcept {
    for (std::uint32_t i = 0; i < tokens; ++i) dispatch_semaphore_signal(semaphore_.get());
}
#else
sleepSemaphore::sleepSemaphore() = default;

sleepSemaphore::~sleepSemaphore() = default;

void sleepSemaphore::acquire() noexcept { semaphore_.acquire(); }

void sleepSemaphore::release(std::uint32_t tokens) noexcept {
    semaphore_.release(static_cast<std::ptrdiff_t>(tokens));
}
#endif

// --- workerPool --------------------------------------------------------------

workerPool::workerPool(int threads, std::function<void()> onStart, std::chrono::microseconds spin)
    : onStart_(std::move(onStart)), spin_(spin), sleep_(workersFor(threads)), busy_(sleep_.size() + 1) {
    const auto workers = sleep_.size();
    // Until a block says otherwise, every worker takes tasks.
    taking_.store(static_cast<std::uint32_t>(busy_.size()), std::memory_order_relaxed);
    workers_.reserve(workers);
    // The members are destroyed once this body throws, so the workers
    // already started are stopped and joined first.
    try {
        for (std::size_t i = 1; i <= workers; ++i) {
            workers_.emplace_back([this, i](const espresso::stop_token& stop) { work(i, stop); });
        }
    } catch (...) {
        stopWorkers();
        throw;
    }
}

void workerPool::takeBusy(std::span<std::uint64_t> out) noexcept ESPRESSO_NONBLOCKING {
    for (std::size_t i = 0; i < out.size() && i < busy_.size(); ++i) {
        out[i] = busy_[i].nanoseconds.exchange(0, std::memory_order_relaxed);
    }
}

workerPool::~workerPool() { stopWorkers(); }

void workerPool::stopWorkers() noexcept {
    // Every request comes before the bump, which carries it to each worker
    // that reads the new generation (see work()), and a token for each
    // worker wakes those that sleep or are about to.
    for (auto& w : workers_) w.request_stop();
    generation_.fetch_add(1, std::memory_order_seq_cst);
    for (auto& s : sleep_) s.release(1);
    workers_.clear();  // joins, before the members the workers use are destroyed
}

void workerPool::run(nodeSchedule& schedule, nodeRunner& runner) noexcept ESPRESSO_NONBLOCKING {
    const taskList& tasks = schedule.tasks;
    // A chain of tasks gains nothing from the workers, and more tasks than
    // the queue's cells would make a push below fail (threads.md §6.1).
    if (runsSerially(tasks)) {
        const auto begin = steadyNow();
        runSerially(schedule, runner);
        const auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(steadyNow() - begin).count();
        busy_[0].nanoseconds.fetch_add(static_cast<std::uint64_t>(took), std::memory_order_relaxed);
        return;
    }
    taskCounters& counters = schedule.counters;
    const auto count = static_cast<std::uint32_t>(tasks.size());
    // Workers past the tasks' thread count take none of them (threads.md
    // §6.1.1). Written only when it changes, since spinning workers poll
    // its cache line.
    if (const auto taking = static_cast<std::uint32_t>(std::min(tasks.threads, threads()));
        taking_.load(std::memory_order_relaxed) != taking) {
        taking_.store(taking, std::memory_order_relaxed);
    }
    tasks_ = tasks;
    counters_ = counters;
    runner_ = runner;
    for (std::uint32_t t = 0; t < count; ++t)
        counters.waiting[t].store(tasks.waits[t], std::memory_order_relaxed);
    left_.store(count, std::memory_order_relaxed);
    // Each push publishes the stores above to the thread that pops it.
    for (std::uint32_t t = 0; t < count; ++t) {
        if (tasks.waits[t] == 0) pushTask(queue_, t);
    }
    wake();
    // Run ready tasks while any are left; with none ready, wait for the
    // tasks the workers are running, at most the longest of them
    // (threads.md §6.4).
    bool waited = false;
    auto finished = steadyNow();
    while (left_.load(std::memory_order_acquire) > 0) {
        if (!runOne(0, finished)) {
            waited = true;
            cpuRelax();
        }
    }
    // No thread holds a task of the block now, so the block's references
    // go back to the inert members without a race: each thread reads them
    // before its release read-modify-write on left_, and the acquire load
    // that read 0 observed the whole release sequence of those writes. The
    // next block's tasks reach the workers only through pushes made after
    // run() sets them again.
    tasks_ = none_;
    counters_ = noCounters_;
    runner_ = idle_;
    if (waited) waited_.fetch_add(1, std::memory_order_relaxed);
}

void workerPool::wake() noexcept ESPRESSO_NONBLOCKING {
    // A worker counts itself sleeping before it reads the generation, so
    // either it sees the new generation or this thread counts it, and gives
    // it a token. A worker counted that saw the new generation first keeps
    // its token for its next sleep, which then ends at once.
    generation_.fetch_add(1, std::memory_order_seq_cst);
    auto sleepers = sleeping_.load(std::memory_order_seq_cst) & takers();
    if (sleepers == 0) return;
    // The release is a system call when a worker sleeps: on Linux a futex
    // wake, in which the kernel takes the futex hash bucket's lock (a
    // sleeping lock on PREEMPT_RT) and wakes the sleeper, and which
    // RealtimeSanitizer intercepts; on Apple platforms a Mach semaphore
    // signal (threads.md §6.5.2). One for each sleeping worker that takes
    // the block's tasks, at most maxThreads − 1.
    ESPRESSO_ASSUME_NONBLOCKING_BEGIN  // a listed departure (modern-cpp skill §9)
    const realtimeSanitizerOff unchecked;
    for (; sleepers != 0; sleepers &= sleepers - 1) {
        sleep_[static_cast<std::size_t>(std::countr_zero(sleepers)) - 1].release(1);
    }
    ESPRESSO_ASSUME_NONBLOCKING_END
}

std::uint64_t workerPool::takers() const noexcept ESPRESSO_NONBLOCKING {
    const auto taking = taking_.load(std::memory_order_relaxed);  // from 1 to maxThreads
    const std::uint64_t below = taking >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << taking) - 1;
    return below & ~std::uint64_t{1};  // bit 0 would be the audio thread's
}

bool workerPool::runOne(std::size_t slot,
                        std::chrono::steady_clock::time_point& finished) noexcept ESPRESSO_NONBLOCKING {
    std::uint32_t t = 0;
    if (!queue_.pop(t)) return false;
    const auto begin = steadyNow();
    const taskList& tasks = tasks_.get();
    taskCounters& counters = counters_.get();
    nodeRunner& runner = runner_.get();
    runner.runAll(tasks.nodesOf(t));
    std::uint32_t queued = 0;
    for (const auto next : tasks.successorsOf(t)) {
        // The last task a successor waits for queues it; the read-modify-
        // write chain carries every waited-for task's writes to it.
        if (counters.waiting[next].fetch_sub(1, std::memory_order_acq_rel) == 1) {
            pushTask(queue_, next);
            ++queued;
        }
    }
    // Two or more tasks queued while a worker sleeps wake it, as a block's
    // start does (threads.md §6.5). One task needs no other thread: this
    // thread looks for a task as soon as it returns, and takes it unless
    // a thread awake takes it first. A worker that counts itself sleeping
    // just after this read misses the wake, which costs parallelism only:
    // this block's tasks still run, on the threads awake and on the audio
    // thread, which runs every task left, and the next wake or block
    // reaches the worker. Reading first keeps the generation's line, which
    // spinning workers read, unwritten while no worker sleeps.
    if (queued >= 2 && (sleeping_.load(std::memory_order_seq_cst) & takers()) != 0) wake();
    finished = steadyNow();
    const auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(finished - begin).count();
    busy_[slot].nanoseconds.fetch_add(static_cast<std::uint64_t>(took), std::memory_order_relaxed);
    // The task's last touch of the block: after it, run() may return.
    left_.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}

void workerPool::work(std::size_t slot, const espresso::stop_token& stop) noexcept {
    nameWorker(slot);
    const denormalGuard guard;
    if (onStart_) onStart_();
    const std::uint64_t bit = std::uint64_t{1} << slot;
    std::uint32_t seen = generation_.load(std::memory_order_acquire);
    auto worked = std::chrono::steady_clock::now();  // the end of the last task, or the last wake
    while (!stop.stop_requested()) {
        // A worker past the block's thread count takes no task and does
        // not spin: it sleeps until a block that needs it wakes it.
        const bool takes = slot < taking_.load(std::memory_order_relaxed);
        if (takes && runOne(slot, worked)) continue;
        if (const auto g = generation_.load(std::memory_order_acquire); g != seen) {
            seen = g;  // a new block, or tasks queued while a worker slept: look for them
            worked = std::chrono::steady_clock::now();
            continue;
        }
        if (takes && std::chrono::steady_clock::now() - worked < spin_) {
            cpuRelax();
            continue;
        }
        // A worker about to sleep misses no stop: stopWorkers() requests
        // every stop before it bumps the generation, then releases a token
        // for every worker. A load here that reads the bump synchronizes
        // with it, so the loop's check that follows sees the request. A
        // load that read an older generation leaves the worker blocked, or
        // about to block, on the semaphore, and the release after the bump
        // gives it a token. A token left by a wake that counted this worker
        // just before it saw the new generation ends a later sleep at once:
        // the worker finds no task and sleeps again.
        sleeping_.fetch_or(bit, std::memory_order_seq_cst);
        if (generation_.load(std::memory_order_seq_cst) == seen) sleep_[slot - 1].acquire();
        sleeping_.fetch_and(~bit, std::memory_order_seq_cst);
    }
}

}  // namespace primitives
