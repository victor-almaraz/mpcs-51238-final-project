/// @file
/// Plans: executable graphs built from a graph description
/// (`primitives.md` §2, §10).

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <proj/metadata/graph.h>
#include <proj/primitives/io.h>
#include <proj/primitives/pipeTable.h>
#include <proj/primitives/primitive.h>

namespace primitives {

/// A node that cannot be built. Thrown on the builder thread only.
class buildError : public std::runtime_error {
public:
    /// An error in node @p node; empty for plan-level errors.
    buildError(std::string node, const std::string& message)
        : std::runtime_error(message), node_(std::move(node)) {}
    /// The failing node's ID.
    const std::string& node() const noexcept { return node_; }

private:
    std::string node_;
};

/// What the builder records about each node for swaps (§9.2) and
/// ordering (§9.3.1).
struct nodeInfo {
    /// Type and port and parameter shape; equal keys allow state adoption.
    std::string shape;
    /// Pipes the node reads: inputs and bound parameters.
    std::vector<metadata::pipeRef> reads;
    /// Pipes the node writes.
    std::vector<metadata::pipeRef> writes;
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
    /// Video-domain primitives, in execution order: the nodes that read or
    /// write a video pipe. The render thread runs them once per frame; the
    /// audio thread never does (§3).
    std::vector<std::shared_ptr<primitive>> videoNodes;
    /// The frame the visual layer shows: the input of the first
    /// `videoOut`, or null.
    std::shared_ptr<const vPipe> display;
    /// Pipe declarations applied when the plan becomes active.
    std::vector<metadata::pipeDecl> pipes;
    /// Every pipe the plan touches, sorted and unique.
    std::vector<metadata::pipeRef> used;
    /// The plan's feedback pipes, sorted.
    std::vector<metadata::pipeRef> feedback;
    /// I/O ports of the plan's sources and sinks, shared with them.
    std::vector<std::shared_ptr<ioPort>> ports;
    /// Tempo to set on activation.
    std::optional<double> tempo;
    /// Beats per bar.
    int meter = 4;

    /// True when pipe @p ref is a feedback pipe in this plan.
    bool isFeedback(metadata::pipeRef ref) const noexcept;
};

/// What building needs from the engine.
struct buildEnv {
    /// The engine's pipes.
    pipeTable& pipes;
    /// The engine's I/O ports.
    ioHub& io;
    /// Sample rate and block size.
    prepareInfo info;
};

/// Builds and prepares a plan from @p graph, which must have passed
/// `ir::validate`. Throws buildError for nodes the engine cannot run,
/// such as an unregistered type or an index beyond the pipe table.
std::shared_ptr<plan> buildPlan(const metadata::graphSpec& graph, std::int32_t planId, buildEnv env);

/// Primitive types the builder can construct.
std::vector<std::string_view> registeredTypes();

} // namespace primitives
