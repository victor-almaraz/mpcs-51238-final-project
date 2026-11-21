// Internal: grouping the nodes of an execution order into tasks
// (threads.md §5). Built on the builder and control threads, never on the
// audio thread. Not part of the public API.

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/plan.h>

namespace primitives {

/// The nodes of an order with dependencies @p deps and estimated costs
/// @p cost (nanoseconds a block each) grouped into tasks. A node joins its
/// only dependency's task when it is that node's only follower (a chain);
/// then tasks at one level of the task graph, which cannot reach each
/// other, join in order while the task they join holds less than
/// @p minimum. Every dependency between tasks is kept, so running each
/// task's nodes in order after the tasks it waits for keeps the order's
/// result.
taskList tasksOf(const dependencyList& deps, std::span<const double> cost,
                 double minimum = defaultMinimumTaskCost);

/// The positions of the nodes of @p deps in a random order that keeps
/// every dependency (threads.md §5.5): each next node is drawn uniformly
/// among those whose dependencies have all been placed, with draws from
/// @p seed alone, so one seed always gives one order.
std::vector<std::uint32_t> shuffledOrder(const dependencyList& deps, std::uint64_t seed);

/// The schedule of @p nodes, run in the order given, with feedback pipes
/// @p feedback (sorted): their dependencies (dependenciesOf), their tasks
/// by each node's estimated cost and @p options' least task cost, the
/// tasks' counters, whether some two tasks can run at once, and, with
/// @p options' seed, a shuffled order for one thread (shuffledOrder).
nodeSchedule scheduleOf(std::span<const std::reference_wrapper<const nodeInfo>> nodes,
                        std::span<const metadata::pipeRef> feedback, const scheduleOptions& options = {});

}  // namespace primitives
