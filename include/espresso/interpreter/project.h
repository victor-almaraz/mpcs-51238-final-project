/// @file
/// The project file, `project.lisp` at the project root
/// (`interpreter.md` §11.1, `grammar.md` §4.4.1): the project's name, its
/// entry piece, its seed, and its page port; and the file a project plays.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
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

/// Result of projectOptions().
struct projectOptionsResult {
    /// The options, when the project file is valid or there is none.
    std::optional<compileOptions> options;
    /// Errors, in the file `projectFile`; empty on success.
    std::vector<compileError> errors;
};

/// The options for compiling a piece of a project: @p options with the
/// entry piece, seed, and page port that the project file declares.
/// @p projectText is the text of the project file, read by readProject();
/// nothing when the project has none, which leaves @p options as they
/// are. Never throws on bad input.
projectOptionsResult projectOptions(compileOptions options, std::optional<std::string_view> projectText);

/// The file a project plays, among @p sources, the project's files by
/// path relative to its root: `main.lisp`; otherwise @p current, the file
/// being edited, when it is among them; otherwise the only one. The
/// project file is never one. Empty when none applies.
std::filesystem::path entryFile(const std::map<std::filesystem::path, std::string>& sources,
                                const std::filesystem::path& current = {});

}  // namespace interpreter
