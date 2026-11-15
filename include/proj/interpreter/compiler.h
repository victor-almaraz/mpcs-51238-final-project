/// @file
/// Compiles a source file to a validated graph (`interpreter.md` §2):
/// read, evaluate, lower mappers, assign node IDs, sort, and validate.
///
/// The supported language is a subset of `grammar.md`: the special forms
/// `define`, `defpiece`, `let` (bindings are sequential), `fn`, `do`,
/// `if`, `cond`, `set!`, `quote`, `module`, `import`, and `pipe`; the
/// object system (`defclass`, `defgeneric`, `defmethod`, `make`); the
/// builtins `+ - * /` with dimensions, `= < > <= >=`, `not`, `list`,
/// `map`, `range`, `tempo`, `mapper`, `slot-value`, and the mapper
/// operations; and calls to every primitive in the `metadata` tables,
/// named in kebab-case. Beat values for time parameters and in mappers
/// are not supported yet.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
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
    /// Most evaluation steps (forms evaluated) before the compilation
    /// fails, so a program that does not end fails instead of hanging.
    std::uint64_t maxSteps = defaultMaxSteps;
    /// Reads the project file at a path relative to the project root, for
    /// `import` (`interpreter.md` §11.2); nothing when there is no such
    /// file. When empty, importing a project file is an error.
    std::function<std::optional<std::string>(const std::string& path)> load;

    /// Default for `maxSteps`. A piece of 64 voices takes about 2,300
    /// steps; a Debug build evaluates about 200,000 steps a second.
    static constexpr std::uint64_t defaultMaxSteps = 5'000'000;
};

/// Most nested evaluations (forms within forms, calls within calls, and
/// mappers of mappers) before a compilation fails with an error. Each
/// level uses C++ stack: about 1.3 KB at most in a Debug build, so the
/// limit needs about 2.6 MB of a thread's stack.
inline constexpr std::size_t maxEvalDepth = 2000;

/// A call that led to an error.
struct callSite {
    /// File of the call, relative to the project root.
    std::string file;
    /// The call.
    sourceSpan span;
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
    std::vector<callSite> stack;

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
