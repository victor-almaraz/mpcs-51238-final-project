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
/// succeeds, so the queue as a whole always makes progress.
class taskQueue {
public:
    /// A queue of at least @p capacity cells, rounded up to a power of two.
    explicit taskQueue(std::size_t capacity);

    /// The number of cells.
    std::size_t capacity() const noexcept { return mask_ + 1; }
    /// Adds @p task; false when the queue is full.
    bool push(std::uint32_t task) noexcept ESPRESSO_NONBLOCKING;
    /// Takes the oldest task into @p task; false when the queue is empty.
    bool pop(std::uint32_t& task) noexcept ESPRESSO_NONBLOCKING;

private:
    struct cell {
        std::atomic<std::size_t> sequence{0};
        std::uint32_t task = 0;
    };
    // Pushers and poppers each update a cache line of their own, apart
    // from the line that both only read.
    alignas(64) std::atomic<std::size_t> enqueue_{0};
    alignas(64) std::atomic<std::size_t> dequeue_{0};
    alignas(64) std::size_t mask_;
    std::vector<cell> cells_;
};

/// The monotonic clock, from the audio thread: it reads the kernel's time
/// page (vDSO, commpage), with no system call.
inline std::chrono::steady_clock::time_point steadyNow() noexcept ESPRESSO_NONBLOCKING {
    ESPRESSO_ASSUME_NONBLOCKING_BEGIN  // no system call: the kernel's time page
    return std::chrono::steady_clock::now();
    ESPRESSO_ASSUME_NONBLOCKING_END
}

/// Waits a moment in a spin loop: the processor's pause or yield hint.
inline void cpuRelax() noexcept ESPRESSO_NONBLOCKING {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

/// The engine's workers: threads that run tasks beside the audio thread.
class workerPool {
public:
    /// The most tasks a block can have; a plan with more runs on the audio
    /// thread alone.
    static constexpr std::size_t maxTasks = std::size_t{1} << 14U;
    /// @p threads − 1 workers, each of which calls @p onStart first on its
    /// own thread (to raise its priority, for example) and spins for
    /// @p spin when it finds no task, then sleeps.
    workerPool(int threads, std::function<void()> onStart,
               std::chrono::microseconds spin = defaultWorkerSpin);

    /// How long a worker spins before it sleeps.
    std::chrono::microseconds spin() const noexcept { return spin_; }
    /// Stops and joins the workers.
    ~workerPool();
    workerPool(const workerPool&) = delete;
    workerPool& operator=(const workerPool&) = delete;
    workerPool(workerPool&&) = delete;
    workerPool& operator=(workerPool&&) = delete;

    /// The threads that run a block, the audio thread included.
    int threads() const noexcept { return static_cast<int>(workers_.size()) + 1; }

    /// Runs every node of @p tasks with @p runner, each task's nodes in
    /// order once the tasks it waits for have finished, on the calling
    /// thread and the workers, and returns when all have run. @p counters
    /// has one counter for each task. Called on the audio thread; the
    /// tasks must be at most maxTasks.
    void run(const taskList& tasks, taskCounters& counters, nodeRunner& runner) noexcept ESPRESSO_NONBLOCKING;

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
    // Worker @p slot's loop.
    void work(std::size_t slot) noexcept;

    // A thread's time running tasks, on a cache line of its own.
    struct alignas(64) busySlot {
        std::atomic<std::uint64_t> nanoseconds{0};
    };

    class idleRunner final : public nodeRunner {
    public:
        void run(std::uint32_t /*node*/) noexcept ESPRESSO_NONBLOCKING override {}
    };

    // In the order that pads least around the two counters every thread
    // writes, each on its cache line. The destructor joins the workers
    // before any member is destroyed, and the constructor starts them once
    // every member exists.
    alignas(64) std::atomic<std::uint32_t> left_{0};  // the block's tasks not yet finished
    std::atomic<std::uint32_t> sleeping_{0};
    idleRunner idle_;  // stands in for a block's runner between blocks
    std::function<void()> onStart_;
    alignas(64) std::atomic<std::uint32_t> generation_{0};  // counts blocks; workers sleep on it
    std::atomic<bool> stopping_{false};
    std::chrono::microseconds spin_;
    // The block being run. run() sets them before it queues the first
    // task, and a thread reads them only while it holds one of the block's
    // tasks: the block cannot end, and run() cannot set them again, until
    // that task has finished. Between blocks they name idle_ and the empty
    // members below; binding a reference before its object is built is
    // allowed, and nothing reads through these until a block runs.
    std::reference_wrapper<const taskList> tasks_{none_};
    std::reference_wrapper<taskCounters> counters_{noCounters_};
    std::reference_wrapper<nodeRunner> runner_{idle_};
    std::atomic<std::uint64_t> waited_{0};
    taskCounters noCounters_;
    std::vector<espresso::jthread> workers_;
    std::vector<busySlot> busy_;  // each thread's time running tasks, written by that thread
    const taskList none_;
    taskQueue queue_{maxTasks};
};

}  // namespace primitives
