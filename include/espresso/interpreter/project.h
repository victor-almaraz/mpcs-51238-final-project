/// @file
/// The project file, `project.lisp` at the project root
/// (`interpreter.md` §11.1, `grammar.md` §4.4.1): the project's name, its
/// entry piece, its seed, and its page port.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/interpreter/compiler.h>

namespace interpreter {

/// The project file's path, relative to the project root.
inline constexpr std::string_view projectFile = "project.lisp";

/// What a project file declares. Each member not given keeps its default.
struct projectSpec {
    /// `:name`: the project's name.
    std::string name;
    /// `:entry`: the piece to build.
    std::string entry = "main";
    /// `:seed`: the seed for random generators (`interpreter.md` §13.7).
    std::uint64_t seed = 0;
    /// `:page-port`: the TCP port of the project's browser front-end
    /// (`io.md` §12.2); none opens no page listener.
    std::optional<int> pagePort;
};

/// Result of reading a project file.
struct projectResult {
    /// The project, when the file is valid.
    std::optional<projectSpec> project;
    /// Errors, in the file `projectFile`; empty on success.
    std::vector<compileError> errors;
};

/// Reads @p source, the text of a project file: one form,
/// `(project :name "drift" :root "." :entry main :seed 7 :page-port 8000)`,
/// whose arguments are all optional. `:root` may only be `"."`, the
/// folder that holds the file. Never throws on bad input.
projectResult readProject(std::string_view source);

/// @p options with the entry piece, seed, and page port that @p project
/// declares.
compileOptions withProject(compileOptions options, const projectSpec& project);

}  // namespace interpreter
