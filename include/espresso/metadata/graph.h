/// @file
/// Graph description shared by the interpreter, the IR, and the engine:
/// pipes, nodes, and parameter values (`interpreter.md` §9,
/// `primitives.md` §6). `ir` reads and writes these types as JSON;
/// `primitives` builds plans from them.

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace metadata {

/// A reference that may be absent: what lookups return instead of a
/// pointer that may be null.
template <typename T>
using optionalRef = std::optional<std::reference_wrapper<const T>>;

/// Pipe types (`primitives.md` §5.1).
enum class pipeKind : std::uint8_t {
    audio,    ///< `aPipe`: sample blocks; writers sum
    control,  ///< `cPipe`: one value per channel per block; writers replace
    event,    ///< `ePipe`: timestamped events; writers merge
    video,    ///< `vPipe`: frames; writers replace
};

/// Lowercase name of @p kind, as used in the IR: `audio`, `control`,
/// `event`, `video`.
std::string_view toString(pipeKind kind) noexcept;

/// The kind named @p name, or nothing.
std::optional<pipeKind> parsePipeKind(std::string_view name) noexcept;

/// Largest channel count of a pipe.
inline constexpr int maxChannels = 8;

/// Index of the engine's tempo pipe, a control pipe in beats per minute
/// (`primitives.md` §7.5). It exists in every plan without a declaration.
inline constexpr std::int32_t tempoPipe = 0;

/// A pipe, identified by type and index (`primitives.md` §6.3).
struct pipeRef {
    /// Pipe type.
    pipeKind kind = pipeKind::audio;
    /// Index within the pipes of that type.
    std::int32_t index = 0;

    /// Ordered by kind, then index.
    friend auto operator<=>(const pipeRef&, const pipeRef&) = default;
};

/// A pipe declared by a graph.
struct pipeDecl {
    /// The pipe.
    pipeRef pipe;
    /// Channel count, from 1 to `maxChannels`. Event pipes have one.
    int channels = 1;
    /// Delays the signal by one block, allowing cycles (`primitives.md` §5.6).
    bool feedback = false;

    /// Equal when every field is equal.
    friend bool operator==(const pipeDecl&, const pipeDecl&) = default;
};

/// A buffer, identified by index (`samples.md` §5.3).
struct bufferRef {
    /// Index within the graph's buffers.
    std::int32_t index = 0;

    /// Ordered by index.
    friend auto operator<=>(const bufferRef&, const bufferRef&) = default;
};

/// Where a node was defined (`interpreter.md` §3.2).
struct sourceSpan {
    /// File path relative to the project root.
    std::string file;
    /// Line, from 1.
    int line = 0;
    /// Column, from 1.
    int column = 0;
    /// Byte offset of the form.
    std::int64_t offset = 0;
    /// Length of the form, in bytes.
    std::int64_t length = 0;

    /// Equal when every field is equal.
    friend bool operator==(const sourceSpan&, const sourceSpan&) = default;
};

/// A buffer declared by a graph (`samples.md` §5.1, §12.2): its length,
/// its file, or both.
struct bufferDecl {
    /// The buffer.
    bufferRef buffer;
    /// Stable ID derived from the source, as node IDs are (`samples.md` §5.2).
    std::string id;
    /// Channel count, from 1 to `maxChannels`.
    int channels = 1;
    /// Length in seconds; without it, the buffer takes its file's length.
    std::optional<double> seconds;
    /// Audio file to load, relative to the project root.
    std::optional<std::string> file;
    /// Whether the file is resampled to the engine's rate (`samples.md`
    /// §4.5.1); when false, its samples stay as they are, at its own rate.
    bool resample = true;
    /// Definition site, for error messages.
    std::optional<sourceSpan> src;

    /// Equal when every field is equal.
    friend bool operator==(const bufferDecl&, const bufferDecl&) = default;
};

/// A parameter value: a number in canonical units, a choice or text, a
/// list of numbers, a bound pipe (`primitives.md` §7.1.2), a buffer
/// (`samples.md` §12.2), or a list of strings (`io.md` §9.1).
using paramValue =
    std::variant<double, std::string, std::vector<double>, pipeRef, bufferRef, std::vector<std::string>>;

/// The alternative @p v holds when it is a T: a checked view instead of
/// `std::get_if`, which returns a pointer.
template <typename T>
optionalRef<T> as(const paramValue& v) noexcept {
    if (!std::holds_alternative<T>(v)) return std::nullopt;
    return std::cref(std::get<T>(v));
}

/// Deleted: the view would outlive the temporary it refers to.
template <typename T>
optionalRef<T> as(const paramValue&&) = delete;

/// One primitive instance in a graph (`interpreter.md` §12.2).
struct nodeSpec {
    /// Stable node ID derived from the source (`interpreter.md` §10).
    std::string id;
    /// Primitive type, such as `sineOsc`.
    std::string type;
    /// Parameters given explicitly; the rest take their defaults.
    std::map<std::string, paramValue, std::less<>> params;
    /// Input ports and the pipes bound to each.
    std::map<std::string, std::vector<pipeRef>, std::less<>> inputs;
    /// Output ports and the pipes bound to each.
    std::map<std::string, std::vector<pipeRef>, std::less<>> outputs;
    /// Definition site, for error messages.
    std::optional<sourceSpan> src;

    /// Equal when every field is equal.
    friend bool operator==(const nodeSpec&, const nodeSpec&) = default;
};

/// Smallest page port (`io.md` §9.3): ports below it need privileges.
inline constexpr int minPagePort = 1024;
/// Largest page port.
inline constexpr int maxPagePort = 65535;

/// True when @p port can be a page port: minPagePort to maxPagePort.
constexpr bool isPagePort(int port) noexcept { return port >= minPagePort && port <= maxPagePort; }

/// A complete graph: what one IR document describes.
struct graphSpec {
    /// Project seed for random generators (`primitives.md` §7.6.4).
    std::uint64_t seed = 0;
    /// Tempo to set when the plan becomes active, in beats per minute.
    std::optional<double> tempo;
    /// Beats per bar, for bar quantization (`primitives.md` §7.5.3).
    int meter = 4;
    /// The page port: the TCP port of the project's browser front-end
    /// (`io.md` §12.2), the IR's `page` object. Without one, the plan
    /// opens no page listener.
    std::optional<int> pagePort;
    /// Declared pipes.
    std::vector<pipeDecl> pipes;
    /// Declared buffers.
    std::vector<bufferDecl> buffers;
    /// Nodes, in execution order.
    std::vector<nodeSpec> nodes;

    /// Equal when every field is equal.
    friend bool operator==(const graphSpec&, const graphSpec&) = default;
};

/// "audio pipe 3", for messages.
std::string toString(pipeRef ref);

/// "buffer 2", for messages.
std::string toString(bufferRef ref);

/// The declaration of @p ref in @p graph, if it has one.
optionalRef<pipeDecl> findPipe(const graphSpec& graph, pipeRef ref) noexcept;

/// Deleted: the reference would outlive the temporary graph.
optionalRef<pipeDecl> findPipe(const graphSpec&&, pipeRef) = delete;

/// The pipe declarations of a graph, sorted by pipe: findPipe() for code
/// that looks up many pipes of one graph. Building the index sorts
/// references to the n declarations once; each lookup is then a binary
/// search, about log2(n) comparisons, against findPipe()'s scan of up to
/// n. The graph must outlive the index, and its pipes must not change.
class pipeIndex {
public:
    /// Indexes the pipes @p graph declares.
    explicit pipeIndex(const graphSpec& graph);
    /// Deleted: the index would refer to a temporary graph.
    explicit pipeIndex(const graphSpec&&) = delete;

    /// The declaration of @p ref, if the graph has one: the first, as
    /// findPipe() finds it, when the graph declares @p ref more than once.
    optionalRef<pipeDecl> find(pipeRef ref) const noexcept;

private:
    /// By pipe; declarations of one pipe in the graph's order.
    std::vector<std::reference_wrapper<const pipeDecl>> sorted_;
};

/// Pipes @p node reads: its inputs, then its parameters bound to pipes.
std::vector<pipeRef> pipesRead(const nodeSpec& node);

/// Pipes @p node writes: its outputs.
std::vector<pipeRef> pipesWritten(const nodeSpec& node);

/// The indices of the nodes of @p graph that write each pipe, in node
/// order. With @p skipFeedback, writers of feedback pipes are left out, as
/// execution order ignores them (`primitives.md` §5.6).
std::map<pipeRef, std::vector<std::size_t>> writersByPipe(const graphSpec& graph, bool skipFeedback);

}  // namespace metadata
