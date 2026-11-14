/// @file
/// Graph description shared by the interpreter, the IR, and the engine:
/// pipes, nodes, and parameter values (`interpreter.md` §9,
/// `primitives.md` §6). `ir` reads and writes these types as JSON;
/// `primitives` builds plans from them.

#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace metadata {

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

/// A parameter value: a number in canonical units, a choice or text, a
/// list of numbers, or a bound pipe (`primitives.md` §7.1.2).
using paramValue = std::variant<double, std::string, std::vector<double>, pipeRef>;

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

/// A complete graph: what one IR document describes.
struct graphSpec {
    /// Project seed for random generators (`primitives.md` §7.6.4).
    std::uint64_t seed = 0;
    /// Tempo to set when the plan becomes active, in beats per minute.
    std::optional<double> tempo;
    /// Beats per bar, for bar quantization (`primitives.md` §7.5.3).
    int meter = 4;
    /// Declared pipes.
    std::vector<pipeDecl> pipes;
    /// Nodes, in execution order.
    std::vector<nodeSpec> nodes;

    /// Equal when every field is equal.
    friend bool operator==(const graphSpec&, const graphSpec&) = default;
};

/// "audio pipe 3", for messages.
std::string toString(pipeRef ref);

/// The declaration of @p ref in @p graph, or null.
const pipeDecl* findPipe(const graphSpec& graph, pipeRef ref) noexcept;

/// Pipes @p node reads: its inputs, then its parameters bound to pipes.
std::vector<pipeRef> pipesRead(const nodeSpec& node);

/// Pipes @p node writes: its outputs.
std::vector<pipeRef> pipesWritten(const nodeSpec& node);

} // namespace metadata
