// The dependencies between the nodes of an execution order (threads.md
// §4.1).

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

}  // namespace

dependencyList dependenciesOf(std::span<const std::reference_wrapper<const nodeInfo>> nodes,
                              std::span<const pipeRef> feedback) {
    // A feedback read takes the other buffer of a double-buffered pipe;
    // event pipes have one buffer (§5.6.2).
    auto delayed = [&](pipeRef r) {
        return (r.kind == pipeKind::audio || r.kind == pipeKind::control) &&
               std::ranges::binary_search(feedback, r);
    };
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
            const auto redirected = std::ranges::find(n.privateWrites, w, &nodeInfo::privateWrite::shared);
            c.write(redirected == n.privateWrites.end() ? pipeAt(w) : privates[redirected->pipe]);
        }
        for (const auto& b : n.bufferWrites) c.write(buffers[b]);
        for (const auto& p : n.ports) c.write(ports[p]);
        if (n.writesDevice) c.write(device);
        if (n.reportsFaults) c.write(faults);
        c.finish(out);
    }
    return out;
}

}  // namespace primitives
