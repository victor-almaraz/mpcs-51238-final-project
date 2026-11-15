// Client entry point: TUI editor and interpreter.
//
// Usage: proj-client [--settings FILE] [--port N] [PROJECT_DIR [FILE...]]
//        proj-client --compile FILE [--root DIR] [--entry NAME] [--seed N]

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#include <proj/interpreter/compiler.h>
#include <proj/ir/ir.h>
#include <proj/protocol/messages.h>
#include <proj/tui/app.h>
#include <proj/tui/fileStore.h>

#include "../arguments.h"

namespace {

void usage(std::ostream& out) {
    out << "usage: proj-client [--settings FILE] [--port N] [PROJECT_DIR [FILE...]]\n"
           "       proj-client --compile FILE [--root DIR] [--entry NAME] [--seed N]\n"
           "\n"
           "Opens the editor on PROJECT_DIR (default: the current directory).\n"
           "FILEs are opened relative to PROJECT_DIR. The server is expected on\n"
           "127.0.0.1:N (default "
        << protocol::defaultServerPort
        << ").\n"
           "--compile writes the IR of FILE's piece (default main) to standard output.\n"
           "Node IDs use FILE's path relative to DIR (default: FILE's directory).\n";
}

// Compiles one file and prints its IR; errors go to standard error.
int compileFile(const std::vector<std::string_view>& args) {
    std::filesystem::path file;
    std::optional<std::filesystem::path> root;
    interpreter::compileOptions options;
    const std::size_t argc = args.size();
    for (std::size_t i = 1; i < argc; ++i) {
        const std::string_view arg = args[i];
        if (arg == "--compile" && i + 1 < argc) {
            file = args[++i];
        } else if (arg == "--root" && i + 1 < argc) {
            root = args[++i];
        } else if (arg == "--entry" && i + 1 < argc) {
            options.entry = std::string(args[++i]);
        } else if (arg == "--seed" && i + 1 < argc) {
            const auto seed = apps::parseNumber<std::uint64_t>(args[++i]);
            if (!seed) {
                std::cerr << "proj-client: invalid --seed " << args[i]
                          << "; it needs a whole number from 0 to "
                          << std::numeric_limits<std::uint64_t>::max() << '\n';
                return 2;
            }
            options.seed = *seed;
        } else {
            std::cerr << "proj-client: invalid argument " << arg << '\n';
            usage(std::cerr);
            return 2;
        }
    }
    const auto text = tui::fileStore::read(file);
    if (!text.text) {
        std::cerr << "proj-client: " << text.error << '\n';
        return 1;
    }
    // The project-relative path, as the editor names the file (node IDs).
    const auto relative = std::filesystem::proximate(file, root ? *root : file.parent_path());
    const auto result = interpreter::compile(*text.text, relative.generic_string(), options);
    const auto base = root ? *root : file.parent_path();
    const std::string prefix = base.empty() ? "" : base.generic_string() + "/";
    if (!result.graph) {  // result.ok(), written so the access below is checked
        for (const auto& e : result.errors) std::cerr << prefix << e.text() << '\n';
        return 1;
    }
    for (const auto& w : result.warnings) {
        std::cerr << prefix << w.file << ':' << w.span.line << ':' << w.span.column << ": warning: " << w.message << '\n';
    }
    std::cout << ir::write(*result.graph, 2) << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const auto args = apps::arguments(argc, argv);
    if (std::ranges::find(args, "--compile") != args.end()) return compileFile(args);
    tui::appOptions options;
    options.link.clientVersion = PROJ_VERSION;
    bool haveRoot = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            return 0;
        }
        if (arg == "--version") {
            std::cout << "proj-client " << PROJ_VERSION << '\n';
            return 0;
        }
        if (arg == "--settings") {
            if (++i >= args.size()) {
                usage(std::cerr);
                return 2;
            }
            options.settingsFile = args[i];
            continue;
        }
        if (arg == "--port") {
            std::optional<std::uint16_t> port;
            if (i + 1 < args.size()) port = apps::parsePort(args[++i]);
            if (!port) {
                std::cerr << "proj-client: --port needs a number from 1 to 65535\n";
                return 2;
            }
            options.link.server.serverPort = *port;
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "proj-client: unknown option " << arg << '\n';
            usage(std::cerr);
            return 2;
        }
        if (!haveRoot) {
            options.projectRoot = args[i];
            haveRoot = true;
        } else {
            options.files.emplace_back(args[i]);
        }
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(options.projectRoot, ec)) {
        std::cerr << "proj-client: " << options.projectRoot.string() << " is not a directory\n";
        return 2;
    }
    tui::app editor(options);
    return editor.run();
}
