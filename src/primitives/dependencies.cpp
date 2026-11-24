// The dependencies between the nodes of an execution order (threads.md
// §4.1), and the work on pipes that falls to each node (§6.8), from one
// pass over the nodes' accesses.

#include "dependencies.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/io.h>
#include <espresso/primitives/pipe.h>
#include <espresso/primitives/plan.h>

namespace primitives {

using metadata::pipeKind;
using metadata::pipeRef;

namespace {

// A resource's accesses since its last write. Its readers since are a
// list through the collector's links, latest first, so that a resource
// allocates nothing.
struct resource {
    std::optional<std::uint32_t> writer;   // the last
    std::optional<std::uint32_t> readers;  // the link of the latest reader since the last write
};

// A pipe's accesses: those the dependencies follow, and the facts that
// place its block work (threads.md §6.8). Its last writer is the
// resource's.
struct pipeSlot {
    resource access;
    bool used = false;                     // listed, or used by a node
    std::optional<std::uint32_t> first;    // the first user other than through a feedback read
    std::optional<std::uint32_t> starter;  // that user, when it writes the pipe
};

// A private pipe's slot, with the writable handle that starts it.
struct privateSlot {
    std::shared_ptr<aPipe> pipe;
    pipeSlot slot;
};

// Collects the accesses of one node at a time. Each adds the earlier
// accesses it conflicts with to the node's dependencies: a write follows
// the last write and every read since; a read follows the last write.
// Earlier conflicts are reached through those.
class collector {
public:
    void start(std::uint32_t node) {
        node_ = node;
        found_.clear();
    }
    void read(resource& r) {
        if (r.writer) found_.push_back(*r.writer);
        links_.push_back({.node = node_, .next = r.readers});
        r.readers = static_cast<std::uint32_t>(links_.size() - 1);
    }
    void write(resource& r) {
        if (r.writer) found_.push_back(*r.writer);
        // Each link leads to an earlier one, so the walk ends.
        for (auto at = r.readers; at; at = links_[*at].next) found_.push_back(links_[*at].node);
        r.writer = node_;
        r.readers.reset();
    }
    void read(pipeSlot& p) {
        p.used = true;
        if (!p.first) p.first = node_;
        read(p.access);
    }
    void write(pipeSlot& p) {
        p.used = true;
        if (!p.first) p.first = node_;
        if (p.first == node_) p.starter = node_;
        write(p.access);
    }
    // A read of a feedback pipe's buffer of the block before: a use that
    // conflicts with nothing.
    static void feedbackRead(pipeSlot& p) { p.used = true; }
    // Appends the node's dependencies, sorted, without itself, to @p out.
    void finish(dependencyList& out) {
        std::ranges::sort(found_);
        found_.erase(std::ranges::unique(found_).begin(), found_.end());
        std::erase(found_, node_);
        out.items.insert(out.items.end(), found_.begin(), found_.end());
        out.start.push_back(static_cast<std::uint32_t>(out.items.size()));
    }

private:
    // A read of a resource, and the read before it since the last write.
    struct link {
        std::uint32_t node = 0;
        std::optional<std::uint32_t> next;
    };

    std::uint32_t node_ = 0;
    std::vector<std::uint32_t> found_;
    std::vector<link> links_;  // every read, in order
};

// True when a read of @p r takes the other buffer of a pipe in
// @p feedback (sorted): a double-buffered pipe's; event pipes have one
// buffer (§5.6.2).
bool delayedRead(pipeRef r, std::span<const pipeRef> feedback) {
    return (r.kind == pipeKind::audio || r.kind == pipeKind::control) &&
           std::ranges::binary_search(feedback, r);
}

// The objects of type T that handles name, each once and ordered by
// identity (std::owner_less), so that an access finds its object's slot
// by binary search instead of a map node a resource: buffers, ports, and
// private pipes are compared by identity, not by index (threads.md
// §4.1.4).
template <typename T>
class identities {
public:
    void add(std::shared_ptr<const T> p) { all_.push_back(std::move(p)); }
    // After the last add(): the count of objects, each one's slot.
    std::size_t seal() {
        std::ranges::sort(all_, std::owner_less<>{});
        const auto same = [](const auto& a, const auto& b) {
            return !a.owner_before(b) && !b.owner_before(a);
        };
        all_.erase(std::ranges::unique(all_, same).begin(), all_.end());
        return all_.size();
    }
    // The slot of the object @p p names, which add() was given.
    template <typename U>
    std::size_t of(const std::shared_ptr<U>& p) const {
        return static_cast<std::size_t>(std::lower_bound(all_.begin(), all_.end(), p, std::owner_less<>{}) -
                                        all_.begin());
    }

private:
    std::vector<std::shared_ptr<const T>> all_;
};

// The private pipe that @p n's write of @p w goes to instead of @p w, if
// any (§4.2): such a write uses that pipe only.
std::optional<std::reference_wrapper<const nodeInfo::privateWrite>> redirected(const nodeInfo& n, pipeRef w) {
    const auto it = std::ranges::find(n.privateWrites, w, &nodeInfo::privateWrite::shared);
    if (it == n.privateWrites.end()) return std::nullopt;
    return std::cref(*it);
}

}  // namespace

// Why a start inside a node is race-free (threads.md §6.8). The node W
// that starts pipe R is the first in the order to use R, and writes it.
// In the dependencies, a write follows the last writer and every read
// since, and a read follows the last writer, apart from feedback reads
// (§4.1.2). So every later writer of R follows W through the chain of
// writers, and every later reader of R follows the last writer before it,
// which is W or follows W. In every run the dependencies admit (the
// block's tasks on the pool, or one thread in plan order or in a shuffled
// order, §5.5), the start, just before W's process(), happens before every
// other use of R in the block, and after every use in the block before,
// which ended before this block's first node ran. Two cases start on the
// audio thread instead. A feedback audio pipe's reads follow no writer,
// and they read the block length that a start sets. A pipe that a node
// reads before the first writer, as a crossfade's merged order can place
// one when it cuts a cycle (primitives.md §9.3.1), needs that read to see
// it started.
orderAnalysis analysisOf(std::span<const std::reference_wrapper<const nodeInfo>> nodes,
                         std::span<const pipeRef> feedback, std::span<const pipeRef> used,
                         std::span<const std::shared_ptr<aPipe>> privates) {
    // Table pipes by kind and index, which are dense and bounded by the
    // pipe table; the other resources by identity, private pipes also in
    // the order they are met.
    std::array<std::vector<pipeSlot>, 3> table;  // audio, control, and event
    auto reach = [&](pipeRef r) {
        auto& kind = table.at(static_cast<std::size_t>(r.kind));
        kind.resize(std::max(kind.size(), static_cast<std::size_t>(r.index) + 1));
    };
    std::ranges::for_each(used, reach);
    for (const nodeInfo& n : nodes) {
        std::ranges::for_each(n.reads, reach);
        std::ranges::for_each(n.writes, reach);
    }
    auto tableAt = [&](pipeRef r) -> pipeSlot& {
        return table.at(static_cast<std::size_t>(r.kind)).at(static_cast<std::size_t>(r.index));
    };
    identities<aPipe> privateIds;
    identities<sampleBuffer> bufferIds;
    identities<ioPort> portIds;
    for (const auto& p : privates) privateIds.add(p);
    for (const nodeInfo& n : nodes) {
        for (const auto& p : n.privateReads) privateIds.add(p);
        for (const auto& w : n.privateWrites) privateIds.add(w.pipe);
        for (const auto& b : n.bufferReads) bufferIds.add(b);
        for (const auto& b : n.bufferWrites) bufferIds.add(b);
        for (const auto& p : n.ports) portIds.add(p);
    }
    // Private pipes by identity, and their slots in the order first met.
    std::vector<privateSlot> own(privateIds.seal());
    std::vector<bool> met(own.size(), false);
    std::vector<std::size_t> metOrder;
    auto privateAt = [&](const auto& p) -> privateSlot& {
        const std::size_t at = privateIds.of(p);
        if (!met[at]) {
            met[at] = true;
            metOrder.push_back(at);
        }
        return own[at];
    };
    for (const auto& p : privates) privateAt(p).pipe = p;
    for (const auto r : used) tableAt(r).used = true;
    std::vector<resource> buffers(bufferIds.seal());
    std::vector<resource> ports(portIds.seal());
    resource device;
    resource faults;

    orderAnalysis out;
    out.dependencies.start.reserve(nodes.size() + 1);
    collector c;
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        const nodeInfo& n = nodes[i];
        c.start(i);
        for (const auto r : n.reads) {
            if (delayedRead(r, feedback)) {
                collector::feedbackRead(tableAt(r));
            } else {
                c.read(tableAt(r));
            }
        }
        for (const auto& b : n.bufferReads) c.read(buffers[bufferIds.of(b)]);
        for (const auto& p : n.privateReads) c.read(privateAt(p).slot);
        // A write that goes to a private pipe touches only that pipe (§4.2).
        for (const auto w : n.writes) {
            if (const auto to = redirected(n, w)) {
                auto& p = privateAt(to->get().pipe);
                p.pipe = to->get().pipe;
                c.write(p.slot);
            } else {
                c.write(tableAt(w));
            }
        }
        for (const auto& b : n.bufferWrites) c.write(buffers[bufferIds.of(b)]);
        for (const auto& p : n.ports) c.write(ports[portIds.of(p)]);
        if (n.writesDevice) c.write(device);
        if (n.reportsFaults) c.write(faults);
        c.finish(out.dependencies);
    }

    pipeWork& work = out.pipes;
    std::vector<nodeItem<pipeRef>> starts;
    std::vector<nodeItem<std::shared_ptr<aPipe>>> privateStarts;
    for (const auto kind : {pipeKind::audio, pipeKind::control, pipeKind::event}) {
        const auto& slots = table.at(static_cast<std::size_t>(kind));
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const pipeSlot& u = slots[i];
            if (!u.used) continue;
            const pipeRef r{.kind = kind, .index = static_cast<std::int32_t>(i)};
            const bool feeds = std::ranges::binary_search(feedback, r);
            if (kind == pipeKind::control) {
                if (feeds) work.serialEnds.push_back(r);  // the other kind with an end
                continue;
            }
            // An event pipe has one buffer, so each read follows its writers
            // whatever the declaration says.
            const bool delayed = kind == pipeKind::audio && feeds;
            if (!delayed && u.starter) {
                starts.push_back({.node = *u.starter, .item = r});
            } else {
                work.serialStarts.push_back(r);
            }
            if (delayed) work.serialEnds.push_back(r);  // a feedback audio pipe swaps
        }
    }
    for (const auto at : metOrder) {
        const privateSlot& p = own[at];
        if (!p.pipe) continue;  // read, written by no node, and not listed: no handle to start it
        if (p.slot.starter) {
            privateStarts.push_back({.node = *p.slot.starter, .item = p.pipe});
        } else {
            work.serialPrivateStarts.push_back(p.pipe);
        }
    }
    work.starts = byNode(nodes.size(), std::move(starts));
    work.privateStarts = byNode(nodes.size(), std::move(privateStarts));
    work.touches.assign(nodes.size(), 0);
    for (std::size_t n = 0; n < nodes.size(); ++n) {
        const auto started = work.privateStarts.of(n);
        const bool pipes = !work.starts.of(n).empty() || !std::ranges::all_of(started, &aPipe::hasTarget);
        const bool forwarding = std::ranges::any_of(started, &aPipe::hasTarget);
        const unsigned bits = (pipes ? unsigned{pipeWork::touchesPipes} : 0U) |
                              (forwarding ? unsigned{pipeWork::touchesForwarding} : 0U);
        work.touches[n] = static_cast<std::uint8_t>(bits);
    }
    return out;
}

}  // namespace primitives
