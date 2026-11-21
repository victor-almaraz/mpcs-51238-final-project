// Internal: grouping the nodes of an execution order into tasks
// (threads.md §5). Built on the builder and control threads, never on the
// audio thread. Not part of the public API.

#pragma once

#include <span>

#include <espresso/primitives/plan.h>

namespace primitives {

/// The least estimated cost a task should hold, in nanoseconds a block,
/// so that a task's bookkeeping, estimated at about a microsecond, stays
/// a few percent of its work (threads.md §5.2, proposed).
inline constexpr double minimumTaskCost = 20'000.0;

/// The nodes of an order with dependencies @p deps and estimated costs
/// @p cost (nanoseconds a block each) grouped into tasks. A node joins its
/// only dependency's task when it is that node's only follower (a chain);
/// then tasks at one level of the task graph, which cannot reach each
/// other, join in order while the task they join holds less than
/// @p minimum. Every dependency between tasks is kept, so running each
/// task's nodes in order after the tasks it waits for keeps the order's
/// result.
taskList tasksOf(const dependencyList& deps, std::span<const double> cost, double minimum = minimumTaskCost);

}  // namespace primitives
