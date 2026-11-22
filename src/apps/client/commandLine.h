// The client's command line without a terminal: its usage, `--compile`,
// `--help`, `--version`, and the editor's options. main() runs the editor
// on the options; everything before it is here, under test.

#pragma once

#include <ostream>
#include <span>
#include <string_view>
#include <variant>

#include <espresso/tui/app.h>

namespace client {

/// Writes the client's usage to @p out.
void usage(std::ostream& out);

/// What a command line leaves to do: open the editor with these options,
/// or exit with this status, the command having run.
using outcome = std::variant<tui::appOptions, int>;

/// Runs the command line @p args, the program's name first, up to the
/// editor. With `--compile FILE [--root DIR] [--entry NAME] [--seed N]`,
/// writes the IR of FILE's piece to @p out, or its errors to @p err, and
/// gives 0, or 1 when FILE or its project file cannot be read or does not
/// compile. `--help` and `--version` write to @p out and give 0. A refused
/// argument, or a project root that is not a directory, is reported on
/// @p err and gives 2. Any other command line gives the editor's options:
/// `[--settings FILE] [--port N] [PROJECT_DIR [FILE...]]`.
outcome run(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace client
