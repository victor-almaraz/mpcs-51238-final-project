/// @file
/// Compiles a source file to a validated graph (`interpreter.md` §2):
/// read, evaluate, lower mappers, assign node IDs, sort, and validate.
///
/// The supported language is a subset of `grammar.md`: the special forms
/// `define`, `defpiece`, `let` (bindings are sequential), `fn`, `do`,
/// `if`, `cond`, `set!`, `quote`, `module`, and `pipe`; the builtins
/// `+ - * /` with dimensions, `= < > <= >=`, `not`, `list`, `map`,
/// `range`, `tempo`, `mapper`, and the mapper operations; and calls to
/// every primitive in the `metadata` tables, named in kebab-case. The
/// object system, `import`, and beat values for time parameters are not
/// supported yet.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <proj/interpreter/reader.h>
#include <proj/metadata/graph.h>

namespace interpreter {

/// Settings for one compilation.
struct compileOptions {
    /// Name of the piece to build (`interpreter.md` §11.1).
    std::string entry = "main";
    /// Project seed, copied into the graph (`interpreter.md` §13.7).
    std::uint64_t seed = 0;
    /// Sample rate for `smp` literals, from the server's `/hello.ok`.
    double sampleRate = 48000.0;
};

/// One compilation error (`interpreter.md` §5.4).
struct compileError {
    /// What is wrong.
    std::string message;
    /// File the error is in, relative to the project root.
    std::string file;
    /// Innermost form.
    sourceSpan span;
    /// Call sites of the closures active at the error, outermost first.
    std::vector<sourceSpan> stack;

    /// One line: `file:line:column: message`.
    std::string text() const;
};

/// Result of a compilation.
struct compileResult {
    /// The validated graph, when compilation succeeded.
    std::optional<metadata::graphSpec> graph;
    /// Errors; empty on success.
    std::vector<compileError> errors;
    /// Likely audio hazards in a graph that compiled: loops that grow,
    /// very large gains, frequencies above half the sample rate.
    std::vector<compileError> warnings;

    /// True when compilation succeeded.
    bool ok() const noexcept { return graph.has_value(); }
    /// The errors, one per line.
    std::string text() const;
};

/// Compiles @p source, the contents of @p file (a path relative to the
/// project root, used in node IDs and errors), and builds the piece
/// named by `options.entry`. Never throws on bad input.
compileResult compile(std::string_view source, std::string_view file, const compileOptions& options = {});

/// The language name of primitive type @p type: `sineOsc` is `sine-osc`.
std::string languageName(std::string_view type);

} // namespace interpreter
