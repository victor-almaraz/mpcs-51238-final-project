// Internal: the dependencies between the nodes of an execution order
// (threads.md §4.1), and the work on pipes they order (§6.8). Built on the
// builder and control threads, never on the audio thread. Not part of the
// public API.

#pragma once

#include <functional>
#include <memory>
#include <span>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/pipe.h>
#include <espresso/primitives/plan.h>

namespace primitives {

/// The dependencies of @p nodes, run in the order given. Two nodes
/// conflict when they use one resource (a pipe, a buffer, an I/O port, or
/// the device's outputs) and at least one writes it; the later then
/// depends on the earlier, so any schedule that keeps the dependencies
/// computes what the order computes. A read of an audio or control pipe in
/// @p feedback, sorted, conflicts with no write: it reads the buffer of the
/// block before (`primitives.md` §5.6.2). A write a node makes to a
/// private pipe (`nodeInfo::privateWrites`) uses that pipe, not the shared
/// one, so writers of one shared pipe conflict only through its summing
/// node.
dependencyList dependenciesOf(std::span<const std::reference_wrapper<const nodeInfo>> nodes,
                              std::span<const metadata::pipeRef> feedback);

/// The work on pipes of a block of @p nodes, run in the order given, with
/// feedback pipes @p feedback (sorted), by the rules of dependenciesOf
/// (threads.md §6.8). The pipes are those @p used lists (sorted and unique;
/// a plan's or a crossfade's `used`), the private pipes @p privates, and
/// every pipe a node uses. A table pipe or a private pipe starts in the
/// node that uses it first, when that node writes it and the pipe is not
/// feedback, and on the audio thread otherwise. A table audio pipe that is
/// not feedback publishes in its last writer, and ends on the audio thread
/// when no node writes it; feedback audio and control pipes end on the
/// audio thread. @p privates must hold every private pipe the nodes read.
pipeWork pipeWorkOf(std::span<const std::reference_wrapper<const nodeInfo>> nodes,
                    std::span<const metadata::pipeRef> feedback, std::span<const metadata::pipeRef> used,
                    std::span<const std::shared_ptr<aPipe>> privates);

}  // namespace primitives
