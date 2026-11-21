// The dependencies between the nodes of an execution order (threads.md
// §4.1), and the work on pipes that falls to each node (§6.8).

#include "dependencies.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
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

// Collects the dependencies of one node at a time: each access adds the
// earlier accesses it conflicts with. A write follows the last write and
// every read since; a read follows the last write. Earlier conflicts are
// reached through those.
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

dependencyList dependenciesOf(std::span<const std::reference_wrapper<const nodeInfo>> nodes,
                              std::span<const pipeRef> feedback) {
    auto delayed = [&](pipeRef r) { return delayedRead(r, feedback); };
    // Table pipes by kind and index, which are dense and bounded by the
    // pipe table; the rest by identity.
    std::array<std::vector<resource>, 4> pipes;
    for (const nodeInfo& n : nodes) {
        for (const auto& list : {std::cref(n.reads), std::cref(n.writes)}) {
            for (const auto r : list.get()) {
                auto& kind = pipes.at(static_cast<std::size_t>(r.kind));
                if (r.index >= 0 && static_cast<std::size_t>(r.index) >= kind.size())
                    kind.resize(static_cast<std::size_t>(r.index) + 1);
            }
        }
    }
    auto pipeAt = [&](pipeRef r) -> resource& {
        return pipes.at(static_cast<std::size_t>(r.kind)).at(static_cast<std::size_t>(r.index));
    };
    std::map<std::shared_ptr<const sampleBuffer>, resource, std::owner_less<>> buffers;
    std::map<std::shared_ptr<const ioPort>, resource, std::owner_less<>> ports;
    std::map<std::shared_ptr<const aPipe>, resource, std::owner_less<>> privates;
    resource device;
    resource faults;

    dependencyList out;
    out.start.reserve(nodes.size() + 1);
    collector c;
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        const nodeInfo& n = nodes[i];
        c.start(i);
        for (const auto r : n.reads) {
            if (!delayed(r)) c.read(pipeAt(r));
        }
        for (const auto& b : n.bufferReads) c.read(buffers[b]);
        for (const auto& p : n.privateReads) c.read(privates[p]);
        // A write that goes to a private pipe touches only that pipe (§4.2).
        for (const auto w : n.writes) {
            const auto to = redirected(n, w);
            c.write(to ? privates[to->get().pipe] : pipeAt(w));
        }
        for (const auto& b : n.bufferWrites) c.write(buffers[b]);
        for (const auto& p : n.ports) c.write(ports[p]);
        if (n.writesDevice) c.write(device);
        if (n.reportsFaults) c.write(faults);
        c.finish(out);
    }
    return out;
}

namespace {

// A pipe's uses in an order: its first user other than through a feedback
// read, that user when it writes the pipe (the node that starts it), and
// its last writer.
struct pipeUses {
    bool used = false;
    std::optional<std::uint32_t> first;
    std::optional<std::uint32_t> starter;
    std::optional<std::uint32_t> lastWriter;

    void read(std::uint32_t node) {
        used = true;
        if (!first) first = node;
    }
    void write(std::uint32_t node) {
        used = true;
        if (!first) first = node;
        if (first == node) starter = node;
        lastWriter = node;
    }
};

// A private pipe's uses, with the pipe itself.
struct privateUses {
    std::shared_ptr<aPipe> pipe;
    pipeUses uses;
};

// An item for one node of an order.
template <typename T>
struct nodeItem {
    std::uint32_t node = 0;
    T item;
};

// The items of @p items, moved out, as lists for each of @p n nodes, each
// node's in the order given: counted, then placed.
template <typename T>
perNode<T> byNode(std::size_t n, std::vector<nodeItem<T>>& items) {
    perNode<T> out;
    out.start.assign(n + 1, 0);
    for (const auto& x : items) ++out.start[x.node + 1];
    std::partial_sum(out.start.begin(), out.start.end(), out.start.begin());
    out.items.resize(items.size());
    std::vector<std::uint32_t> next(out.start.begin(), out.start.end() - 1);
    for (auto& x : items) out.items[next[x.node]++] = std::move(x.item);
    return out;
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
pipeWork pipeWorkOf(std::span<const std::reference_wrapper<const nodeInfo>> nodes,
                    std::span<const pipeRef> feedback, std::span<const pipeRef> used,
                    std::span<const std::shared_ptr<aPipe>> privates) {
    const auto n = static_cast<std::uint32_t>(nodes.size());
    // Table pipes by kind and index, as in dependenciesOf; private pipes in
    // the order they are met, by identity.
    std::array<std::vector<pipeUses>, 4> table;
    auto tableAt = [&](pipeRef r) -> std::optional<std::reference_wrapper<pipeUses>> {
        if (r.index < 0) return std::nullopt;  // no table pipe: a built plan has none
        auto& kind = table.at(static_cast<std::size_t>(r.kind));
        const auto i = static_cast<std::size_t>(r.index);
        if (i >= kind.size()) kind.resize(i + 1);
        return std::ref(kind[i]);
    };
    std::vector<privateUses> own;
    std::map<std::shared_ptr<const aPipe>, std::size_t, std::owner_less<>> ownIndex;
    auto privateAt = [&](const auto& p) -> privateUses& {
        if (const auto it = ownIndex.find(p); it != ownIndex.end()) return own[it->second];
        ownIndex.emplace(p, own.size());
        return own.emplace_back();
    };
    for (const auto& p : privates) privateAt(p).pipe = p;
    for (const auto r : used) {
        if (const auto u = tableAt(r)) u->get().used = true;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        const nodeInfo& node = nodes[i];
        for (const auto r : node.reads) {
            const auto u = tableAt(r);
            if (!u) continue;
            if (delayedRead(r, feedback)) {
                u->get().used = true;
            } else {
                u->get().read(i);
            }
        }
        for (const auto& p : node.privateReads) privateAt(p).uses.read(i);
        for (const auto w : node.writes) {
            if (const auto to = redirected(node, w)) {
                auto& p = privateAt(to->get().pipe);
                if (!p.pipe) p.pipe = to->get().pipe;
                p.uses.write(i);
            } else if (const auto u = tableAt(w)) {
                u->get().write(i);
            }
        }
    }

    pipeWork out;
    std::vector<nodeItem<pipeRef>> starts;
    std::vector<nodeItem<std::shared_ptr<aPipe>>> privateStarts;
    std::vector<nodeItem<pipeRef>> publishes;
    for (const auto kind : {pipeKind::audio, pipeKind::control, pipeKind::event}) {
        const auto& uses = table.at(static_cast<std::size_t>(kind));
        for (std::size_t i = 0; i < uses.size(); ++i) {
            const pipeUses& u = uses[i];
            if (!u.used) continue;
            const pipeRef r{.kind = kind, .index = static_cast<std::int32_t>(i)};
            const bool feeds = std::ranges::binary_search(feedback, r);
            if (kind == pipeKind::control) {
                if (feeds) out.serialEnds.push_back(r);  // the other kind with an end
                continue;
            }
            // An event pipe has one buffer, so each read follows its writers
            // whatever the declaration says.
            const bool delayed = kind == pipeKind::audio && feeds;
            if (!delayed && u.starter) {
                starts.push_back({.node = *u.starter, .item = r});
            } else {
                out.serialStarts.push_back(r);
            }
            if (kind != pipeKind::audio) continue;
            if (!delayed && u.lastWriter) {
                publishes.push_back({.node = *u.lastWriter, .item = r});
            } else {
                out.serialEnds.push_back(r);
            }
        }
    }
    for (const auto& p : own) {
        assert(p.pipe);  // every private pipe read is in privates or written
        if (!p.pipe) continue;
        if (p.uses.starter) {
            privateStarts.push_back({.node = *p.uses.starter, .item = p.pipe});
        } else {
            out.serialPrivateStarts.push_back(p.pipe);
        }
    }
    out.starts = byNode(n, starts);
    out.privateStarts = byNode(n, privateStarts);
    out.publishes = byNode(n, publishes);
    return out;
}

}  // namespace primitives
