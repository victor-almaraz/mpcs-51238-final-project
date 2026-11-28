/// @file
/// Plans: executable graphs built from a graph description
/// (`primitives.md` §2, §10).

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/clip.h>
#include <espresso/primitives/io.h>
#include <espresso/primitives/pipeTable.h>
#include <espresso/primitives/primitive.h>
#include <espresso/primitives/realtime.h>

namespace primitives {

/// A node that cannot be built. Thrown on the builder thread only.
class buildError : public std::runtime_error {
public:
    /// What an error is about.
    enum class subject : std::uint8_t {
        node,    ///< A node, or the plan as a whole when the ID is empty
        buffer,  ///< A buffer declaration (`samples.md` §4.6)
    };

    /// An error in node @p node; empty for plan-level errors.
    buildError(std::string node, const std::string& message)
        : std::runtime_error(message), node_(std::move(node)) {}
    /// An error about @p what with ID @p id.
    buildError(subject what, std::string id, const std::string& message)
        : std::runtime_error(message), node_(std::move(id)), about_(what) {}
    /// The failing node's or buffer's ID.
    const std::string& node() const noexcept { return node_; }
    /// Whether node() names a node or a buffer.
    subject about() const noexcept { return about_; }

private:
    std::string node_;
    subject about_ = subject::node;
};

/// What the builder records about each node for swaps (§9.2), ordering
/// (§9.3.1), and dependencies (`threads.md` §4.1).
struct nodeInfo {
    /// Type and port and parameter shape; equal keys allow state adoption.
    std::string shape;
    /// Pipes the node reads: inputs and bound parameters.
    std::vector<metadata::pipeRef> reads;
    /// Pipes the node writes.
    std::vector<metadata::pipeRef> writes;
    /// Buffers the node only reads (`samples.md` §5.7).
    std::vector<std::shared_ptr<const sampleBuffer>> bufferReads;
    /// Buffers the node writes.
    std::vector<std::shared_ptr<const sampleBuffer>> bufferWrites;
    /// The I/O ports the node uses; any two users of a port depend on each
    /// other.
    std::vector<std::shared_ptr<const ioPort>> ports;
    /// True when the node adds to the device's outputs (`audioOut`).
    bool writesDevice = false;
    /// True when the node reports faults (`ioHub::reportFault`), whose
    /// queue takes one producer at a time: every two such nodes depend on
    /// each other (`threads.md` §4.1).
    bool reportsFaults = false;

    /// A write of `writes` that goes to a private pipe of the plan instead
    /// (`threads.md` §4.2).
    struct privateWrite {
        /// The pipe the node's IR names.
        metadata::pipeRef shared;
        /// The pipe it writes, which a summing node adds into `shared`;
        /// writable, as the block's start of it falls to this node
        /// (pipeWork).
        std::shared_ptr<aPipe> pipe;
    };
    /// The node's writes that go to private pipes.
    std::vector<privateWrite> privateWrites;
    /// The private pipes a summing node reads, in the order it adds them.
    std::vector<std::shared_ptr<const aPipe>> privateReads;
    /// The node's estimated cost, in nanoseconds a block (`threads.md`
    /// §5.2).
    double cost = 0.0;

    /// True for a summing node, which the builder adds and which alone
    /// reads private pipes (`threads.md` §4.2): in a crossfade it never
    /// adopts state and never ramps.
    bool sums() const noexcept { return !privateReads.empty(); }
};

/// Items for each node of an execution order, or for each task of a task
/// list, node by node (or task by task) in one array.
template <typename T>
struct perNode {
    /// Where each node's items start in `items`; one entry more than there
    /// are nodes.
    std::vector<std::uint32_t> start{0};
    /// The items of every node, node by node.
    std::vector<T> items;

    /// The number of nodes.
    std::size_t size() const noexcept { return start.size() - 1; }
    /// The items of node @p i, which is less than size().
    std::span<const T> of(std::size_t i) const noexcept {
        return std::span(items).subspan(start[i], start[i + 1] - start[i]);
    }
};

/// The nodes of an execution order grouped into tasks (`threads.md` §5):
/// a thread runs a task's nodes in order, start to finish, once every
/// task it waits for has finished.
struct taskList {
    /// The nodes of each task, by position in the order, each task's in
    /// execution order.
    perNode<std::uint32_t> nodes;
    /// The tasks that wait for each task, each task's sorted.
    perNode<std::uint32_t> successors;
    /// The number of tasks each task waits for.
    std::vector<std::uint32_t> waits;
    /// The estimated work of a block run in order on one thread, in
    /// nanoseconds a block: the sum of the nodes' estimated costs
    /// (`nodeInfo::cost`, `threads.md` §5.2).
    double serialWork = 0.0;
    /// The estimated work of a block run as tasks, in nanoseconds a block:
    /// the sum of the tasks' costs. A task costs its nodes' estimated costs
    /// and a fixed cost for each dependency of one of them on a node of
    /// another task, whose output its thread must read from another
    /// processor's cache (`threads.md` §6.1.1).
    double work = 0.0;
    /// The estimated critical path, in nanoseconds a block: the costliest
    /// path through the task graph by the tasks' costs. No number of
    /// threads runs the tasks in less.
    double criticalPath = 0.0;
    /// True when the tasks run on the engine's threads: some level of the
    /// task graph holds two tasks, which can run at once, and the
    /// estimated saving at the engine's thread count (savingAt) reaches
    /// `scheduleOptions::minimumParallelSaving`. The engine runs tasks
    /// that are not parallel on the audio thread alone, in
    /// `nodeSchedule::serialOrder`, waking no worker (`threads.md` §6.1.1).
    bool parallel = false;
    /// The most threads that run the tasks when they are parallel, the
    /// audio thread included (`threads.md` §6.1.1). tasksOf sets it to the
    /// fewest of the engine's threads whose estimated saving falls short of
    /// the saving on all of them by less than
    /// `scheduleOptions::minimumParallelSaving`, when that is fewer than
    /// all: the threads past it would save less than that together, so
    /// they sleep through the block instead of taking a share of the cores
    /// from the threads that work. Otherwise, and in lists built by hand,
    /// every thread of the pool runs them.
    int threads = std::numeric_limits<int>::max();

    /// The estimated saving, in nanoseconds a block, of running the tasks
    /// on @p count threads instead of in order on one: `serialWork` less
    /// the larger of `criticalPath` and `work` / @p count, the least time
    /// @p count threads can take; 0 when that is negative, and for one
    /// thread.
    double savingAt(int count) const noexcept {
        return count > 1 ? std::max(0.0, serialWork - std::max(criticalPath, work / count)) : 0.0;
    }

    /// The number of tasks.
    std::size_t size() const noexcept { return nodes.size(); }
};

/// What the scheduler counts down in each block (`threads.md` §6.1): for
/// each task, the tasks it still waits for. Allocated with the plan and
/// written only while it runs.
struct taskCounters {
    static_assert(realtimeAtomic<std::uint32_t>);

    /// One counter for each task.
    std::vector<std::atomic<std::uint32_t>> waiting;
};

/// For each node of an execution order, the earlier nodes it must run
/// after (`threads.md` §4.1), by position in the order, each node's
/// sorted. Every node a node must follow is reachable through the list,
/// though not always directly: a node after two writers of a pipe lists
/// only the second, which follows the first.
using dependencyList = perNode<std::uint32_t>;

/// The work a block does on its pipes (`threads.md` §6.8), placed so that
/// most of it runs inside the tasks: each audio and event pipe of the
/// table, and each private pipe, starts (zeroed or emptied for the block)
/// just before the node that first uses it in the order runs, when that
/// node writes it. The audio thread does the rest, before and after every
/// node. Built on the builder thread with the
/// order's schedule; the audio thread and the workers only read it.
///
/// A start in a node is race-free: the node is the first in the order to
/// use the pipe, and every later user follows it through the dependencies
/// (`threads.md` §4.1.2), in every order the dependencies admit. Feedback
/// pipes, whose reads follow no writer, start and end on the audio thread.
struct pipeWork {
    /// The table's audio and event pipes each node starts before it runs.
    perNode<metadata::pipeRef> starts;
    /// The private pipes each node starts before it runs.
    perNode<std::shared_ptr<aPipe>> privateStarts;
    /// For each node, the work it does on pipes, as bits: one byte the
    /// runner reads for the many nodes that do none. `touchesPipes` when it
    /// starts a table pipe or a private pipe without a
    /// target; `touchesForwarding` when it starts a private pipe with one
    /// (aPipe::hasTarget). A plan whose private pipes forward starts none
    /// of those (`threads.md` §4.2.5), so its runner skips a node with
    /// only the second bit without reading its pipes.
    std::vector<std::uint8_t> touches;
    /// The bit of `touches` for a table pipe's start, or a start of a
    /// private pipe without a target.
    static constexpr std::uint8_t touchesPipes = 1;
    /// The bit of `touches` for a start of a private pipe with a target.
    static constexpr std::uint8_t touchesForwarding = 2;
    /// The table's audio and event pipes the audio thread starts before any
    /// node runs: feedback pipes, pipes no node writes, and pipes a node
    /// reads before the first writes them.
    std::vector<metadata::pipeRef> serialStarts;
    /// The private pipes the audio thread starts before any node runs:
    /// those no node of the order writes (in a crossfade, those of writers
    /// the incoming plan adopted) and those a node reads before their
    /// writer runs.
    std::vector<std::shared_ptr<aPipe>> serialPrivateStarts;
    /// The pipes the audio thread ends after every node has run
    /// (pipeTable::endBlock): feedback audio and control pipes, which swap
    /// their buffers.
    std::vector<metadata::pipeRef> serialEnds;
};

/// How the nodes of an execution order may run in parallel: their
/// dependencies, their tasks, and the scheduler's counters for the tasks.
struct nodeSchedule {
    /// The dependencies of the nodes, in their order (`threads.md` §4.1).
    dependencyList dependencies;
    /// The nodes grouped into tasks (`threads.md` §5).
    taskList tasks;
    /// The scheduler's counters for `tasks`.
    taskCounters counters;
    /// The order in which a thread that runs every node of a block alone
    /// runs them (an engine of one thread, or tasks that are not parallel
    /// or do not fit the pool): the positions in their own order, or, with
    /// `scheduleOptions::shuffleSeed`, a permutation of them that keeps
    /// every dependency (`threads.md` §5.5).
    std::vector<std::uint32_t> serialOrder;
    /// The block's work on pipes, by node (`threads.md` §6.8).
    pipeWork pipes;
};

/// The least estimated cost of a task by default, in nanoseconds a block
/// (`threads.md` §5.2): a task's bookkeeping, about a
/// microsecond, then stays a few percent of its work.
inline constexpr double defaultMinimumTaskCost = 20'000.0;

/// The least estimated saving for which a schedule runs in parallel by
/// default, in nanoseconds a block (`scheduleOptions::minimumParallelSaving`,
/// `threads.md` §6.1.1). In Release renders of every example at 2 threads
/// on the reference machine (Intel Core i5-7360U), each example that
/// renders slower in parallel is estimated to save nothing, and the one
/// estimated to save least while rendering faster saves 7.5 µs.
inline constexpr double defaultMinimumParallelSaving = 4'000.0;

/// How the builder makes the schedules of plans and crossfades
/// (`engineOptions::schedule`). The output is the same at every setting.
struct scheduleOptions {
    /// The least estimated cost of a task, in nanoseconds a block, not
    /// negative (`threads.md` §5.2): the builder joins chains of nodes that
    /// cannot reach each other into one task until it holds this much. At
    /// 0 every chain is a task of its own, the finest partition, which
    /// tests use so that independent nodes run on different threads.
    double minimumTaskCost = defaultMinimumTaskCost;
    /// The threads that run a block, the audio thread included: the
    /// engine sets it to `engineOptions::threads`. At 1 no schedule is
    /// parallel (taskList::parallel).
    int threads = 1;
    /// The least estimated saving a block, in nanoseconds, not negative,
    /// for which tasks that can run at once run in parallel on `threads`
    /// threads (taskList::savingAt, taskList::parallel); with less, the
    /// block runs in order on the audio thread. At 0 every schedule whose
    /// tasks can run at once is parallel, which tests use to run every
    /// plan on the pool; an infinite one makes every schedule serial.
    double minimumParallelSaving = defaultMinimumParallelSaving;
    /// For testing that the dependencies capture every conflict
    /// (`threads.md` §5.5, §9.1): when set, a block whose nodes one thread
    /// runs alone (one thread, or tasks that are not parallel or do not fit
    /// the pool) runs them in a random order that keeps every dependency
    /// (`nodeSchedule::serialOrder`), drawn from this seed alone once for
    /// each plan and each crossfade, instead of plan order. A node that
    /// touches shared state it does not declare then renders differently.
    std::optional<std::uint64_t> shuffleSeed = std::nullopt;
};

/// An executable graph: primitive instances in execution order, the
/// pipes they use, and their I/O ports. Shared between the engine's
/// threads; its last owner is always on the control thread.
class plan {
public:
    /// Plan ID assigned by the client.
    std::int32_t id = 0;
    /// Primitives, in execution order (§10.1). Shared with the transitions
    /// that run them during a fade.
    std::vector<std::shared_ptr<primitive>> nodes;
    /// Per-node data, parallel to `nodes`.
    std::vector<nodeInfo> info;
    /// The private audio pipes of the plan's writers (`threads.md` §4.2),
    /// started each block like the table's (nodeSchedule::pipes).
    std::vector<std::shared_ptr<aPipe>> privatePipes;
    /// How `nodes` may run in parallel (`threads.md` §4, §5).
    nodeSchedule schedule;
    /// True when the plan, running alone, runs its blocks in plan order,
    /// so that its private pipes forward (`threads.md` §4.2.5): on an
    /// engine of one thread, which has no pool, or with a schedule the
    /// pool runs serially, and with no shuffle seed, which can reorder the
    /// writers of one pipe. Set by buildPlan with `schedule`, for the
    /// engine whose options it was built with.
    bool runsInPlanOrder = false;
    /// Seconds each node has spent in process(), parallel to `nodes`, when
    /// the engine profiles (`engineOptions::profile`); written by the thread
    /// that runs the node, the audio thread or a worker, and read once the
    /// plan has stopped.
    std::vector<double> seconds;
    /// The buffers the plan declares, in declaration order, held for as
    /// long as the plan lives (`samples.md` §3.1).
    std::vector<std::shared_ptr<sampleBuffer>> buffers;
    /// Pipe declarations applied when the plan becomes active.
    std::vector<metadata::pipeDecl> pipes;
    /// Every pipe the plan touches except the tempo pipe, which persists
    /// and is never cleared or swapped; sorted and unique.
    std::vector<metadata::pipeRef> used;
    /// The plan's feedback pipes, sorted.
    std::vector<metadata::pipeRef> feedback;
    /// I/O ports of the plan's sources and sinks, shared with them.
    std::vector<std::shared_ptr<ioPort>> ports;
    /// Tempo to set on activation.
    std::optional<double> tempo;
    /// Beats per bar.
    int meter = 4;
};

/// What building needs from the engine.
struct buildEnv {
    /// The engine's pipes.
    pipeTable& pipes;
    /// The engine's I/O ports.
    ioHub& io;
    /// Sample rate and block size.
    prepareInfo info;
    /// The engine's live buffers, for adoption and the memory bound.
    bufferPool& buffers;
    /// Decodes audio files; may be empty.
    const sampleLoader& loadSample;
    /// The engine's clip banks (`canvas.md` §5.1); without them, each node
    /// that plays clips has a bank of its own.
    std::optional<std::reference_wrapper<clipBanks>> clips = std::nullopt;
    /// How the plan's schedule is made.
    scheduleOptions schedule = {};
};

/// Builds and prepares a plan from @p graph, which must have passed
/// `ir::validate`. Throws buildError for nodes the engine cannot run,
/// such as an unregistered type or an index beyond the pipe table, and
/// for buffers it cannot make: a file that does not load, or more buffer
/// memory than the pool allows (`samples.md` §4.6, §5.6).
std::shared_ptr<plan> buildPlan(const metadata::graphSpec& graph, std::int32_t planId, buildEnv env);

/// Primitive types the builder can construct.
std::vector<std::string_view> registeredTypes();

}  // namespace primitives
