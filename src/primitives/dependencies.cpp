// The dependencies between the nodes of an execution order (threads.md
// §4.1), and the work on pipes that falls to each node (§6.8), from one
// pass over the nodes' accesses.

#include "dependencies.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
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

// A resource's accesses since its last write.
struct resource {
    std::optional<std::uint32_t> writer;  // the last
    std::vector<std::uint32_t> readers;   // since the last write
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
        r.readers.push_back(node_);
    }
    void write(resource& r) {
        if (r.writer) found_.push_back(*r.writer);
        found_.insert(found_.end(), r.readers.begin(), r.readers.end());
        r.writer = node_;
        r.readers.clear();
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
        out.before.insert(out.before.end(), found_.begin(), found_.end());
        out.start.push_back(static_cast<std::uint32_t>(out.before.size()));
    }

private:
    std::uint32_t node_ = 0;
    std::vector<std::uint32_t> found_;
};

// True when a read of @p r takes the other buffer of a pipe in
// @p feedback (sorted): a double-buffered pipe's; event pipes have one
// buffer (§5.6.2).
bool delayedRead(pipeRef r, std::span<const pipeRef> feedback) {
    return (r.kind == pipeKind::audio || r.kind == pipeKind::control) &&
           std::ranges::binary_search(feedback, r);
}

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
// it started. The last writer L of a pipe follows every other writer, so
// L's publish reads the block's final samples; nodes that run beside it
// only read them.
orderAnalysis analysisOf(std::span<const std::reference_wrapper<const nodeInfo>> nodes,
                         std::span<const pipeRef> feedback, std::span<const pipeRef> used,
                         std::span<const std::shared_ptr<aPipe>> privates) {
    // Table pipes by kind and index, which are dense and bounded by the
    // pipe table; private pipes in the order they are met; the other
    // resources by identity.
    std::array<std::vector<pipeSlot>, 4> table;
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
    std::vector<privateSlot> own;
    std::map<std::shared_ptr<const aPipe>, std::size_t, std::owner_less<>> ownIndex;
    auto privateAt = [&](const auto& p) -> privateSlot& {
        if (const auto it = ownIndex.find(p); it != ownIndex.end()) return own[it->second];
        ownIndex.emplace(p, own.size());
        return own.emplace_back();
    };
    for (const auto& p : privates) privateAt(p).pipe = p;
    for (const auto r : used) tableAt(r).used = true;
    std::map<std::shared_ptr<const sampleBuffer>, resource, std::owner_less<>> buffers;
    std::map<std::shared_ptr<const ioPort>, resource, std::owner_less<>> ports;
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
        for (const auto& b : n.bufferReads) c.read(buffers[b]);
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
        for (const auto& b : n.bufferWrites) c.write(buffers[b]);
        for (const auto& p : n.ports) c.write(ports[p]);
        if (n.writesDevice) c.write(device);
        if (n.reportsFaults) c.write(faults);
        c.finish(out.dependencies);
    }

    pipeWork& work = out.pipes;
    std::vector<nodeItem<pipeRef>> starts;
    std::vector<nodeItem<std::shared_ptr<aPipe>>> privateStarts;
    std::vector<nodeItem<pipeRef>> publishes;
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
            if (kind != pipeKind::audio) continue;
            if (!delayed && u.access.writer) {
                publishes.push_back({.node = *u.access.writer, .item = r});
            } else {
                work.serialEnds.push_back(r);
            }
        }
    }
    for (const auto& p : own) {
        if (!p.pipe) continue;  // read, written by no node, and not listed: no handle to start it
        if (p.slot.starter) {
            privateStarts.push_back({.node = *p.slot.starter, .item = p.pipe});
        } else {
            work.serialPrivateStarts.push_back(p.pipe);
        }
    }
    work.starts = byNode(nodes.size(), std::move(starts));
    work.privateStarts = byNode(nodes.size(), std::move(privateStarts));
    work.publishes = byNode(nodes.size(), std::move(publishes));
    return out;
}

}  // namespace primitives
