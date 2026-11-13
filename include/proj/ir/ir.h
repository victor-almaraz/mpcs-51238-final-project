/// @file
/// IR reading, writing, and validation (`interpreter.md` §12). The IR is
/// JSON; `schema/ir.schema.json` defines its structure and the
/// primitive tables in `metadata` define its meaning.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <proj/metadata/graph.h>

namespace ir {

/// Value of the IR's `format` field.
inline constexpr std::string_view formatName = "proj-ir";
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

    /// One line: `file:line:column: node ID: message`, omitting the
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

/// Parses @p json, validates it against the schema, and checks it with
/// validate(). Never throws on bad input.
readResult read(std::string_view json);

/// Serializes @p graph as an IR document. @p indent is passed to the
/// JSON writer: -1 for compact output.
std::string write(const metadata::graphSpec& graph, int indent = -1);

/// Semantic checks (`interpreter.md` §12.3): known types and parameters,
/// value kinds and ranges, declared pipes of matching types, required
/// ports, unique node IDs, and execution order (every reader of a pipe
/// after all of its writers, except through feedback pipes).
std::vector<error> validate(const metadata::graphSpec& graph);

/// The text of `schema/ir.schema.json`, embedded at build time.
std::string_view schema() noexcept;

} // namespace ir
