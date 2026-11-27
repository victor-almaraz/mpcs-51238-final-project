// The client's command line without a terminal (commandLine.h).

#include "commandLine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <espresso/interpreter/compiler.h>
#include <espresso/interpreter/project.h>
#include <espresso/ir/ir.h>
#include <espresso/page/render.h>
#include <espresso/protocol/messages.h>
#include <espresso/tui/app.h>
#include <espresso/workspace/fileStore.h>

#include "../arguments.h"
#include "serve.h"

namespace client {

namespace {

// Compiles one file and writes its IR to @p out, or with @p page the page
// drawn from its layout (dsl-ui.md §8); errors go to @p err.
int compileFile(std::span<const std::string_view> args, std::ostream& out, std::ostream& err, bool page) {
    std::filesystem::path file;
    std::optional<std::filesystem::path> root;
    std::optional<std::string> entry;
    std::optional<std::uint64_t> seed;
    const std::size_t argc = args.size();
    for (std::size_t i = 1; i < argc; ++i) {
        const std::string_view arg = args[i];
        // Every option of --compile takes a value; anything else is refused.
        const auto value = apps::nextValue(args, i);
        if ((arg == (page ? "--page" : "--compile")) && value) {
            file = *value;
        } else if (arg == "--root" && value) {
            root = *value;
        } else if (arg == "--entry" && value) {
            entry = std::string(*value);
        } else if (arg == "--seed" && value) {
            seed = apps::parseNumber<std::uint64_t>(*value);
            if (!seed) {
                err << "espresso-client: invalid --seed " << *value << "; it needs a whole number from 0 to "
                    << std::numeric_limits<std::uint64_t>::max() << '\n';
                return 2;
            }
        } else {
            err << "espresso-client: invalid argument " << arg << '\n';
            usage(err);
            return 2;
        }
    }
    const auto text = workspace::fileStore::read(file);
    if (!text.text) {
        err << "espresso-client: " << text.error << '\n';
        return 1;
    }
    const auto base = root ? *root : file.parent_path();
    // DIR's project file gives the entry, seed, and page port; --entry and
    // --seed override it (interpreter.md §11.1).
    std::optional<std::string> projectText;
    const auto projectPath = base / interpreter::projectFile;
    if (std::error_code missing; std::filesystem::exists(projectPath, missing)) {
        auto read = workspace::fileStore::read(projectPath);
        if (!read.text) {
            err << "espresso-client: " << read.error << '\n';
            return 1;
        }
        projectText = std::move(read.text);
    }
    // The project-relative path, as the editor names the file (node IDs).
    const auto relative = std::filesystem::proximate(file, base);
    const auto compiled = interpreter::compileProject(
        *text.text, relative.generic_string(), {}, projectText,
        [base](const std::string& path) {
            auto read = workspace::fileStore::read(base / path);
            return interpreter::loadedFile{.text = std::move(read.text), .error = std::move(read.error)};
        },
        {.entry = entry, .seed = seed});
    if (!compiled.projectErrors.empty()) {
        for (const auto& e : compiled.projectErrors) {
            err << (base / e.file).generic_string() << ':' << e.span.line << ':' << e.span.column << ": "
                << e.message << '\n';
        }
        return 1;
    }
    const auto& result = compiled.piece;
    // Project files are named from DIR; standard library files are not in it.
    const auto print = [&base, &err](const interpreter::compileError& e, std::string_view tag) {
        const bool inProject = !base.empty() && !interpreter::isLibraryPath(e.file);
        err << (inProject ? (base / e.file).generic_string() : e.file) << ':' << e.span.line << ':'
            << e.span.column << ": " << tag << e.message << '\n';
    };
    if (!result.graph) {  // result.ok(), written so the access below is checked
        for (const auto& e : result.errors) print(e, "");
        return 1;
    }
    for (const auto& w : result.warnings) print(w, "warning: ");
    if (!page) {
        out << ir::write(*result.graph, 2) << '\n';
        return 0;
    }
    if (!result.graph->layout) {
        err << "espresso-client: " << file.generic_string()
            << "'s piece has no page; its controls (knob, slider, ...) or a (page ...) make one\n";
        return 1;
    }
    out << page::render(*result.graph->layout, false);
    return 0;
}

// The port of `--port N` at args[i], which it steps past; or nothing,
// the refusal written to @p err.
std::optional<std::uint16_t> portOption(std::span<const std::string_view> args, std::size_t& i,
                                        std::ostream& err) {
    const auto value = apps::nextValue(args, i);
    const auto port = value ? apps::parsePort(*value) : std::nullopt;
    if (!port) err << "espresso-client: --port needs a number from 1 to 65535\n";
    return port;
}

// The sidecar's options, or the exit status of a refused argument.
outcome sidecarOptions(std::span<const std::string_view> args, std::ostream& err) {
    serveOptions options;
    options.link.clientVersion = ESPRESSO_VERSION;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--serve") continue;
        if (arg == "--port") {
            const auto port = portOption(args, i, err);
            if (!port) return 2;
            options.link.server.serverPort = *port;
            continue;
        }
        err << "espresso-client: invalid argument " << arg << " with --serve\n";
        usage(err);
        return 2;
    }
    return options;
}

// The editor's options, or the exit status of --help, --version, or a
// refused argument.
outcome editorOptions(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
    tui::appOptions options;
    options.link.clientVersion = ESPRESSO_VERSION;
    bool haveRoot = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--help" || arg == "-h") {
            usage(out);
            return 0;
        }
        if (arg == "--version") {
            out << "espresso-client " << ESPRESSO_VERSION << '\n';
            return 0;
        }
        if (arg == "--settings") {
            const auto value = apps::nextValue(args, i);
            if (!value) {
                usage(err);
                return 2;
            }
            options.settingsFile = *value;
            continue;
        }
        if (arg == "--port") {
            const auto port = portOption(args, i, err);
            if (!port) return 2;
            options.link.server.serverPort = *port;
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            err << "espresso-client: unknown option " << arg << '\n';
            usage(err);
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
        err << "espresso-client: " << options.projectRoot.string() << " is not a directory\n";
        return 2;
    }
    return options;
}

}  // namespace

void usage(std::ostream& out) {
    out << "usage: espresso-client [--settings FILE] [--port N] [PROJECT_DIR [FILE...]]\n"
           "       espresso-client --compile FILE [--root DIR] [--entry NAME] [--seed N]\n"
           "       espresso-client --page FILE [--root DIR] [--entry NAME] [--seed N]\n"
           "       espresso-client --serve [--port N]\n"
           "\n"
           "Opens the editor on PROJECT_DIR (default: the current directory).\n"
           "FILEs are opened relative to PROJECT_DIR. The server is expected on\n"
           "127.0.0.1:N (default "
        << protocol::defaultServerPort
        << ").\n"
           "--compile writes the IR of FILE's piece (default main) to standard output.\n"
           "DIR's project.lisp, when there is one, names the piece, the seed, and\n"
           "the page port; --entry and --seed override it.\n"
           "Node IDs use FILE's path relative to DIR (default: FILE's directory),\n"
           "and import paths resolve against DIR.\n"
           "--page writes the page drawn from the piece's layout, the page the server\n"
           "serves without ui/index.html, to standard output: written to\n"
           "DIR/ui/index.html, it is the project's own page to edit.\n"
           "--serve answers an editor's requests, JSON-RPC 2.0 messages one a line,\n"
           "on standard input and output, in place of the terminal editor.\n";
}

outcome run(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
    if (std::ranges::find(args, "--compile") != args.end()) return compileFile(args, out, err, false);
    if (std::ranges::find(args, "--page") != args.end()) return compileFile(args, out, err, true);
    if (std::ranges::find(args, "--serve") != args.end()) return sidecarOptions(args, err);
    return editorOptions(args, out, err);
}

}  // namespace client
