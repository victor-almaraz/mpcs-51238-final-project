// Internal: the dependencies between the nodes of an execution order
// (threads.md §4.1). Built on the builder and control threads, never on
// the audio thread. Not part of the public API.

#pragma once

#include <functional>
#include <span>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/plan.h>

namespace primitives {

/// The dependencies of @p nodes, run in the order given. Two nodes
/// conflict when they use one resource (a pipe, a buffer, an I/O port, or
/// the device's outputs) and at least one writes it; the later then
/// depends on the earlier, so any schedule that keeps the dependencies
/// computes what the order computes. A read of an audio or control pipe in
/// @p feedback, sorted, conflicts with no write: it reads the buffer of the
/// block before (`primitives.md` §5.6.2).
dependencyList dependenciesOf(std::span<const std::reference_wrapper<const nodeInfo>> nodes,
                              std::span<const metadata::pipeRef> feedback);

}  // namespace primitives
