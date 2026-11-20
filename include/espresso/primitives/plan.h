/// @file
/// Plans: executable graphs built from a graph description
/// (`primitives.md` §2, §10).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/frame.h>
#include <espresso/primitives/io.h>
#include <espresso/primitives/pipeTable.h>
#include <espresso/primitives/primitive.h>

namespace primitives {

/// A node that cannot be built. Thrown on the builder thread only.
class buildError : public std::runtime_error {
public:
    /// What an error is about.
    enum class subject : std::uint8_t {
        node,    ///< A node, or the plan as a whole when the ID is empty
        buffer,  ///< A buffer declaration (`samples.md` §4.6)
    };

    /// An error in node @p node; empty for plan-level errors.
    buildError(std::string node, const std::string& message)
        : std::runtime_error(message), node_(std::move(node)) {}
    /// An error about @p what with ID @p id.
    buildError(subject what, std::string id, const std::string& message)
        : std::runtime_error(message), node_(std::move(id)), about_(what) {}
    /// The failing node's or buffer's ID.
    const std::string& node() const noexcept { return node_; }
    /// Whether node() names a node or a buffer.
    subject about() const noexcept { return about_; }

private:
    std::string node_;
    subject about_ = subject::node;
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
    /// Seconds each node has spent in process(), parallel to `nodes`, when
    /// the engine profiles (`engineOptions::profile`); written by the audio
    /// thread only, and read once the plan has stopped (`threads.md` §11).
    std::vector<double> seconds;
    /// Video-domain primitives, in execution order: the nodes that read or
    /// write a video pipe. The render thread runs them once per frame; the
    /// audio thread never does (§3).
    std::vector<std::shared_ptr<primitive>> videoNodes;
    /// The frame the visual layer shows: the input of the first
    /// `videoOut`, or null.
    std::shared_ptr<const vPipe> display;
    /// The buffers the plan declares, in declaration order, held for as
    /// long as the plan lives (`samples.md` §3.1).
    std::vector<std::shared_ptr<sampleBuffer>> buffers;
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
    /// The page port of the graph it was built from (`io.md` §12.2): the
    /// server listens on it while the plan is active.
    std::optional<int> pagePort;

    /// True when pipe @p ref is a feedback pipe in this plan.
    bool isFeedback(metadata::pipeRef ref) const noexcept;
};

/// Decodes the image file at @p path. Returns no image, with @p error set,
/// when it cannot.
using imageLoader = std::function<std::optional<image>(std::string_view path, std::string& error)>;

/// What building needs from the engine.
struct buildEnv {
    /// The engine's pipes.
    pipeTable& pipes;
    /// The engine's I/O ports.
    ioHub& io;
    /// Sample rate and block size.
    prepareInfo info;
    /// Decodes image files; may be empty.
    const imageLoader& loadImage;
    /// The engine's live buffers, for adoption and the memory bound.
    bufferPool& buffers;
    /// Decodes audio files; may be empty.
    const sampleLoader& loadSample;
};

/// Builds and prepares a plan from @p graph, which must have passed
/// `ir::validate`. Throws buildError for nodes the engine cannot run,
/// such as an unregistered type or an index beyond the pipe table, and
/// for buffers it cannot make: a file that does not load, or more buffer
/// memory than the pool allows (`samples.md` §4.6, §5.6).
std::shared_ptr<plan> buildPlan(const metadata::graphSpec& graph, std::int32_t planId, buildEnv env);

/// Primitive types the builder can construct.
std::vector<std::string_view> registeredTypes();

}  // namespace primitives
