// Internal: the engine's worker threads and the scheduler that runs a
// block's tasks on them and on the audio thread (threads.md §6, §7). Not
// part of the public API.

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <span>
#include <vector>

#if defined(__APPLE__)
#include <memory>
#include <type_traits>

#include <dispatch/dispatch.h>
#else
#include <semaphore>
#endif

#include <espresso/primitives/engine.h>
#include <espresso/primitives/plan.h>
#include <espresso/primitives/realtime.h>
#include <espresso/protocol/jthread.h>

namespace primitives {

/// Runs one node of a block.
class nodeRunner {
public:
    /// Runs node @p node, by position in the order the tasks group.
    virtual void run(std::uint32_t node) noexcept ESPRESSO_NONBLOCKING = 0;
    /// Runs @p nodes in turn: a task's nodes, or a serial order. A runner
    /// overrides it to run them without a virtual call each.
    virtual void runAll(std::span<const std::uint32_t> nodes) noexcept ESPRESSO_NONBLOCKING {
        for (const auto node : nodes) run(node);
    }

protected:
    nodeRunner() = default;
    nodeRunner(const nodeRunner&) = default;
    nodeRunner& operator=(const nodeRunner&) = default;
    nodeRunner(nodeRunner&&) = default;
    nodeRunner& operator=(nodeRunner&&) = default;
    ~nodeRunner() = default;
};

/// A bounded queue of task indices for any number of threads that push and
/// pop (Vyukov's bounded MPMC queue). Every cell is allocated at
/// construction. A push or pop retries only while another thread's
/// succeeds, but the queue is not lock-free: a pusher stalled between
/// claiming a cell and publishing it makes every pop report the queue
/// empty at that cell until it publishes, though later cells hold tasks.
/// The stall stays a few instructions long only while every thread that
/// pushes runs at the audio thread's priority (threads.md §6.3, §6.6).
class taskQueue {
public:
    /// A queue of at least @p capacity cells, rounded up to a power of two.
    explicit taskQueue(std::size_t capacity);

    /// The number of cells.
    std::size_t capacity() const noexcept { return mask_ + 1; }
    /// Adds @p task; false when the queue is full.
    [[nodiscard]] bool push(std::uint32_t task) noexcept ESPRESSO_NONBLOCKING;
    /// Takes the oldest task into @p task; false when the queue is empty.
    [[nodiscard]] bool pop(std::uint32_t& task) noexcept ESPRESSO_NONBLOCKING;

private:
    static_assert(realtimeAtomic<std::size_t>);
    struct cell {
        std::atomic<std::size_t> sequence{0};
        std::uint32_t task = 0;
    };
    // Pushers and poppers each update a cache line of their own, apart
    // from the line that both only read.
    alignas(cacheLine) std::atomic<std::size_t> enqueue_{0};
    alignas(cacheLine) std::atomic<std::size_t> dequeue_{0};
    alignas(cacheLine) std::size_t mask_;
    std::vector<cell> cells_;
};

/// The monotonic clock, from the audio thread: it reads the kernel's time
/// page (vDSO, commpage), with no system call.
inline std::chrono::steady_clock::time_point steadyNow() noexcept ESPRESSO_NONBLOCKING {
    ESPRESSO_ASSUME_NONBLOCKING_BEGIN  // no system call: the kernel's time page
    return std::chrono::steady_clock::now();
    ESPRESSO_ASSUME_NONBLOCKING_END
}

/// Waits a moment in a spin loop: `pause` on x86, `isb` on AArch64. Most
/// AArch64 cores retire `yield` as a no-op, so a loop on it polls at full
/// rate; an instruction barrier waits for the pipeline to drain, a real
/// delay, as in Rust's `spin_loop`, OpenJDK, and folly.
inline void cpuRelax() noexcept ESPRESSO_NONBLOCKING {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("isb");
#endif
}

/// The counting semaphore idle workers sleep on (threads.md §6.5.4): a
/// release adds tokens, and an acquire takes one, waiting while none is
/// left. std::counting_semaphore, which on Linux waits on a futex, except
/// on Apple platforms, whose libc++ waits through std::atomic::wait by
/// polling with sleeps of up to half the time already waited (8 ms past
/// 128 ms): a worker asleep for 60 ms woke 28 ms after its release. There
/// it is libdispatch's semaphore, which wakes a waiter in microseconds.
class sleepSemaphore {
public:
    /// No tokens. Throws std::system_error when the system has no
    /// semaphore to give.
    sleepSemaphore();
    ~sleepSemaphore();
    sleepSemaphore(const sleepSemaphore&) = delete;
    sleepSemaphore& operator=(const sleepSemaphore&) = delete;
    sleepSemaphore(sleepSemaphore&&) = delete;
    sleepSemaphore& operator=(sleepSemaphore&&) = delete;

    /// Takes a token, waiting for a release while none is left.
    void acquire() noexcept;
    /// Adds @p tokens tokens, waking as many waiters. A system call when a
    /// thread waits (threads.md §6.5.2).
    void release(std::uint32_t tokens) noexcept;

private:
#if defined(__APPLE__)
    struct releaseSemaphore {
        void operator()(dispatch_semaphore_t s) const noexcept { dispatch_release(s); }
    };
    // A libdispatch object by pointer, which C++ releases by hand
    // (modern-cpp skill §9).
    std::unique_ptr<std::remove_pointer_t<dispatch_semaphore_t>, releaseSemaphore> semaphore_;
#else
    std::counting_semaphore<> semaphore_{0};
#endif
};

/// The engine's workers: threads that run tasks beside the audio thread.
class workerPool {
public:
    /// The most tasks a block can have; a plan with more runs on the audio
    /// thread alone.
    static constexpr std::size_t maxTasks = std::size_t{1} << 14U;
    /// @p threads − 1 workers, for @p threads up to maxThreads, each of
    /// which names its thread `espresso-w<N>`, calls @p onStart on it (to
    /// raise its priority, for example), and spins for @p spin when it
    /// finds no task, then sleeps.
    ///
    /// @p onStart runs first on each worker's own thread, inside a noexcept
    /// function, so it must not throw: a throw ends the process. It may
    /// still be running when the constructor returns, and the destructor
    /// waits for it to return. It must own what it captures, since the
    /// pool's copy can outlive the objects in the caller's scope.
    ///
    /// When a worker cannot start, the constructor stops and joins the
    /// workers it started, then rethrows: std::system_error when the
    /// system refuses the thread or a semaphore. More than maxThreads
    /// throws std::invalid_argument.
    workerPool(int threads, std::function<void()> onStart,
               std::chrono::microseconds spin = defaultWorkerSpin);

    /// How long a worker spins before it sleeps.
    std::chrono::microseconds spin() const noexcept { return spin_; }
    /// Stops and joins the workers, waiting for a start hook still running.
    ~workerPool();
    workerPool(const workerPool&) = delete;
    workerPool& operator=(const workerPool&) = delete;
    workerPool(workerPool&&) = delete;
    workerPool& operator=(workerPool&&) = delete;

    /// The threads that run a block, the audio thread included.
    int threads() const noexcept { return static_cast<int>(workers_.size()) + 1; }

    /// True when run() runs the nodes of a schedule with @p tasks on the
    /// calling thread alone, in its serial order: the tasks are not
    /// parallel (taskList::parallel), or more than maxTasks.
    static bool runsSerially(const taskList& tasks) noexcept ESPRESSO_NONBLOCKING {
        return !tasks.parallel || tasks.size() > maxTasks;
    }

    /// Runs every node of @p schedule with @p runner and returns when all
    /// have run. When its tasks can run in parallel (taskList::parallel)
    /// and fit (maxTasks), each task's nodes run in order once the tasks it
    /// waits for have finished, on the calling thread and the first
    /// workers, up to the tasks' thread count (taskList::threads); the
    /// other workers take no task and are not woken. Otherwise the calling
    /// thread runs the nodes alone, in the schedule's serial order
    /// (nodeSchedule::serialOrder), waking no worker, and adds the time to
    /// its time running tasks (threads.md §5.5, §6.1, §8.2). Called on the
    /// audio thread.
    void run(nodeSchedule& schedule, nodeRunner& runner) noexcept ESPRESSO_NONBLOCKING;

    /// Moves each thread's time running tasks since the last call, in
    /// nanoseconds, into @p out (the audio thread first, then each worker),
    /// and zeroes it (threads.md §8). @p out has threads() entries. Called
    /// on the audio thread.
    void takeBusy(std::span<std::uint64_t> out) noexcept ESPRESSO_NONBLOCKING;
    /// The blocks in which the audio thread found no task ready while a
    /// worker was still running one (threads.md §8).
    std::uint64_t waitedBlocks() const noexcept { return waited_.load(std::memory_order_relaxed); }

private:
    // Takes one ready task and runs it on thread @p slot (0 for the audio
    // thread), adding its time to the slot and setting @p finished to when
    // it ended; false when none was ready.
    bool runOne(std::size_t slot,
                std::chrono::steady_clock::time_point& finished) noexcept ESPRESSO_NONBLOCKING;
    // Starts a new generation, so that a worker about to sleep looks for
    // tasks again and a spinning one restarts its spin, and wakes the
    // workers that sleep among those that take the block's tasks, if any
    // (threads.md §6.5).
    void wake() noexcept ESPRESSO_NONBLOCKING;
    // The bits of the workers that take the block's tasks in sleeping_:
    // workers 1 to taking_ − 1.
    std::uint64_t takers() const noexcept ESPRESSO_NONBLOCKING;
    // Worker @p slot's loop, until @p stop reports a stop request.
    void work(std::size_t slot, const espresso::stop_token& stop) noexcept;
    // Asks every worker to stop, wakes the sleeping ones, and joins them.
    void stopWorkers() noexcept;

    // A thread's time running tasks, on a cache line of its own.
    struct alignas(cacheLine) busySlot {
        std::atomic<std::uint64_t> nanoseconds{0};
    };

    class idleRunner final : public nodeRunner {
    public:
        void run(std::uint32_t /*node*/) noexcept ESPRESSO_NONBLOCKING override {}
    };

    // The pool's counters, and the flag of each worker's stop token.
    static_assert(realtimeAtomic<std::uint32_t> && realtimeAtomic<std::uint64_t> && realtimeAtomic<bool>);
    // Each worker's bit in sleeping_, from bit 1.
    static_assert(maxThreads <= 64);

    // Three groups that threads write during a block each start a cache
    // line, so that a write to one leaves the others' lines valid: left_
    // and sleeping_, which every thread updates as it finishes a task or
    // sleeps; generation_ and taking_, which spinning workers poll; and the
    // block's references with waited_, which run() writes at each block's
    // start and end. The members after each group are only read during a
    // block, apart from busy_'s slots and the queue's counters, which have
    // lines of their own. The destructor, and a constructor that cannot
    // start a worker, join the workers before any member is destroyed; the
    // constructor starts them once every member exists.
    alignas(cacheLine) std::atomic<std::uint32_t> left_{0};  // the block's tasks not yet finished
    // Bit w is set while worker w counts itself sleeping.
    std::atomic<std::uint64_t> sleeping_{0};
    idleRunner idle_;  // stands in for a block's runner between blocks
    std::function<void()> onStart_;
    // Counts wakes: each block's start, each queueing of tasks while a
    // worker slept, and the stop. A worker sleeps on its semaphore only
    // while it reads the generation it last saw, and each wake releases a
    // token to every worker that takes the block's tasks and counts itself
    // sleeping.
    alignas(cacheLine) std::atomic<std::uint32_t> generation_{0};
    // The threads that take the last parallel block's tasks, the audio
    // thread included (taskList::threads): workers 1 to taking_ − 1. The
    // others take no task and do not spin, so they sleep until a block
    // needs them. Written by run() before it queues a block's first task,
    // and only when it changes; a worker reads it as a hint. One that reads
    // an old count takes a task of a block that does not need it, or
    // sleeps until the next wake, and the block's tasks still all run.
    std::atomic<std::uint32_t> taking_{0};
    std::chrono::microseconds spin_;
    std::vector<sleepSemaphore> sleep_;  // worker w's at w − 1
    // The block being run. run() sets them before it queues the first
    // task and restores them once every task has finished, and a thread
    // reads them only while it holds one of the block's tasks: the block
    // cannot end, and run() cannot set them again, until that task has
    // finished. Between blocks they name idle_ and the empty members
    // below; binding a reference before its object is built is allowed,
    // and nothing reads through these until a block runs.
    alignas(cacheLine) std::reference_wrapper<const taskList> tasks_{none_};
    std::reference_wrapper<taskCounters> counters_{noCounters_};
    std::reference_wrapper<nodeRunner> runner_{idle_};
    std::atomic<std::uint64_t> waited_{0};
    taskCounters noCounters_;
    std::vector<espresso::jthread> workers_;
    std::vector<busySlot> busy_;  // each thread's time running tasks, written by that thread
    const taskList none_;
    taskQueue queue_{maxTasks};
};

/// Runs every node of @p schedule with @p runner on the calling thread, in
/// the schedule's serial order (nodeSchedule::serialOrder): the engine of
/// one thread, which has no pool (threads.md §3.3, §5.5). Called on the
/// audio thread.
void runSerially(const nodeSchedule& schedule, nodeRunner& runner) noexcept ESPRESSO_NONBLOCKING;

}  // namespace primitives
