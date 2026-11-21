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
#include <vector>

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
    // Pushers and poppers each update their own cache line.
    alignas(64) std::atomic<std::size_t> enqueue_{0};
    std::size_t mask_;
    std::vector<cell> cells_;
    alignas(64) std::atomic<std::size_t> dequeue_{0};
};

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
    /// How long a worker that finds no task spins before it sleeps
    /// (threads.md §6.5, proposed).
    static constexpr std::chrono::microseconds spinTime{50};

    /// @p threads − 1 workers, each of which calls @p onStart first on its
    /// own thread (to raise its priority, for example).
    workerPool(int threads, std::function<void()> onStart);
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

private:
    // Takes one ready task and runs it; false when none was ready.
    bool runOne() noexcept ESPRESSO_NONBLOCKING;
    // A worker's loop.
    void work() noexcept;

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
    // The block being run. run() sets them before it queues the first
    // task, and a thread reads them only while it holds one of the block's
    // tasks: the block cannot end, and run() cannot set them again, until
    // that task has finished. Between blocks they name idle_ and the empty
    // members below; binding a reference before its object is built is
    // allowed, and nothing reads through these until a block runs.
    std::reference_wrapper<const taskList> tasks_{none_};
    std::reference_wrapper<taskCounters> counters_{noCounters_};
    std::reference_wrapper<nodeRunner> runner_{idle_};
    taskCounters noCounters_;
    std::vector<espresso::jthread> workers_;
    const taskList none_;
    taskQueue queue_{maxTasks};
};

}  // namespace primitives
