// Internal: the dependencies between the nodes of an execution order
// (threads.md §4.1), and the work on pipes they order (§6.8). Built on the
// builder and control threads, never on the audio thread. Not part of the
// public API.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/pipe.h>
#include <espresso/primitives/plan.h>

namespace primitives {

/// An item for node `node` of an order.
template <typename T>
struct nodeItem {
    /// The node, by position in the order.
    std::uint32_t node = 0;
    /// The item.
    T item;
};

/// @p items, moved out, as lists for each of @p n nodes, each node's in
/// the order given: counted, then placed (a counting sort).
template <typename T>
perNode<T> byNode(std::size_t n, std::vector<nodeItem<T>>&& items) {
    perNode<T> out;
    out.start.assign(n + 1, 0);
    for (const auto& x : items) ++out.start[x.node + 1];
    std::partial_sum(out.start.begin(), out.start.end(), out.start.begin());
    out.items.resize(items.size());
    std::vector<std::uint32_t> next(out.start.begin(), out.start.end() - 1);
    for (auto& x : items) out.items[next[x.node]++] = std::move(x.item);
    return out;
}

/// What one pass over the accesses of an order's nodes finds.
struct orderAnalysis {
    /// The dependencies between the nodes.
    dependencyList dependencies;
    /// The block's work on pipes, by node.
    pipeWork pipes;
};

/// The dependencies of @p nodes, run in the order given, and the block's
/// work on their pipes, from one pass over each node's accesses.
///
/// Two nodes conflict when they use one resource (a pipe, a buffer, an I/O
/// port, or the device's outputs) and at least one writes it; the later
/// then depends on the earlier, so any schedule that keeps the
/// dependencies computes what the order computes. A read of an audio or
/// control pipe in @p feedback, sorted, conflicts with no write: it reads
/// the buffer of the block before (`primitives.md` §5.6.2). A write a node
/// makes to a private pipe (`nodeInfo::privateWrites`) uses that pipe, not
/// the shared one, so writers of one shared pipe conflict only through its
/// summing node.
///
/// The pipes of the work (threads.md §6.8) are those @p used lists (sorted
/// and unique; a plan's or a crossfade's `used`), every pipe a node uses,
/// and the private pipes @p privates lists. A table pipe or a private pipe
/// starts in the node that uses it first, when that node writes it and the
/// pipe is not feedback, and on the audio thread otherwise. A table audio
/// pipe that is not feedback publishes in its last writer, and ends on the
/// audio thread when no node writes it; feedback audio and control pipes
/// end on the audio thread. A private pipe starts through its writer's
/// handle (`nodeInfo::privateWrite::pipe`), or through @p privates' when no
/// node writes it, on the audio thread; one that nodes only read and
/// @p privates does not list does not start. Only a crossfade has such
/// pipes, and lists its outgoing plan's (threads.md §6.8.3).
orderAnalysis analysisOf(std::span<const std::reference_wrapper<const nodeInfo>> nodes,
                         std::span<const metadata::pipeRef> feedback,
                         std::span<const metadata::pipeRef> used = {},
                         std::span<const std::shared_ptr<aPipe>> privates = {});

}  // namespace primitives
