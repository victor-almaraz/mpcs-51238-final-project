/// @file
/// IR reading, writing, and validation (`interpreter.md` §12). The IR is
/// JSON; `schema/ir.schema.json` defines its structure and the
/// primitive tables in `metadata` define its meaning.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/metadata/graph.h>

/// IR JSON reading and writing, schema and semantic validation, data-flow
/// sorting, and hazard analysis (the `ir` module).
namespace ir {

/// Value of the IR's `format` field.
inline constexpr std::string_view formatName = "espresso-ir";
/// Value of the IR's `version` field.
inline constexpr int formatVersion = 1;

/// One problem found in an IR document (`interpreter.md` §12.3).
struct error {
    /// ID of the failing node; empty for document-level errors.
    std::string node;
    /// What is wrong.
    std::string message;
    /// Definition site of the failing node, when the IR carries it.
    std::optional<metadata::sourceSpan> src;

    /// One line: `path:line:column: node ID: message`, omitting the
    /// parts that are absent.
    std::string text() const;
};

/// Result of reading an IR document.
struct readResult {
    /// The graph, when the document is valid.
    std::optional<metadata::graphSpec> graph;
    /// Every problem found; empty when the document is valid.
    std::vector<error> errors;

    /// True when the document is valid.
    bool ok() const noexcept { return graph.has_value(); }
    /// The errors, one per line.
    std::string text() const;
};

/// Parses @p text, IR as JSON, validates it against the schema, and checks
/// it with validate(). Never throws on bad input.
readResult read(std::string_view text);

/// Serializes @p graph as an IR document. @p indent is passed to the
/// JSON writer: -1 for compact output.
std::string write(const metadata::graphSpec& graph, int indent = -1);

/// Semantic checks (`interpreter.md` §12.3): known types and parameters,
/// value kinds and ranges, declared pipes of matching types, required
/// ports, unique node IDs, and execution order (every reader of a pipe
/// after all of its writers, except through feedback pipes).
std::vector<error> validate(const metadata::graphSpec& graph);

/// Sorts the nodes of @p graph so every writer of a pipe precedes its
/// readers, except through feedback pipes; ties keep the original order.
/// When the graph has a cycle, leaves it unchanged and returns the
/// original indices of the nodes on cycles, in order.
std::vector<std::size_t> sortByDataFlow(metadata::graphSpec& graph);

/// Likely audio hazards in a valid, sorted graph, for warnings: a loop
/// through audio feedback pipes with a gain of 1 or more once around
/// (estimated from static `gain`, `adsr` and `mix` levels per input
/// position, along every path through inputs and parameters bound to
/// audio pipes, including paths through other feedback pipes; parallel
/// paths summed; when a node or parameter of unknown gain is on the loop,
/// only an estimate above 1 warns), one warning per set of feedback pipes
/// that reach each other; constant audio gains above +24 dB; and constant
/// frequencies above half of @p sampleRate.
std::vector<error> hazards(const metadata::graphSpec& graph, double sampleRate);

/// The text of `schema/ir.schema.json`, embedded at build time.
std::string_view schema() noexcept;

}  // namespace ir
