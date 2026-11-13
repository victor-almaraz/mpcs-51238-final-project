// Client entry point: TUI editor and interpreter.
//
// Usage: proj-client [--settings FILE] [--port N] [PROJECT_DIR [FILE...]]
//        proj-client --compile FILE [--entry NAME] [--seed N]

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

#include <proj/interpreter/compiler.h>
#include <proj/ir/ir.h>
#include <proj/protocol/messages.h>
#include <proj/tui/app.h>
#include <proj/tui/fileStore.h>

namespace {

void usage(std::ostream& out) {
    out << "usage: proj-client [--settings FILE] [--port N] [PROJECT_DIR [FILE...]]\n"
           "       proj-client --compile FILE [--entry NAME] [--seed N]\n"
           "\n"
           "Opens the editor on PROJECT_DIR (default: the current directory).\n"
           "FILEs are opened relative to PROJECT_DIR. The server is expected on\n"
           "127.0.0.1:N (default "
        << protocol::defaultServerPort
        << ").\n"
           "--compile writes the IR of FILE's piece (default main) to standard output.\n";
}

// Compiles one file and prints its IR; errors go to standard error.
int compileFile(int argc, char** argv) {
    std::filesystem::path file;
    interpreter::compileOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--compile" && i + 1 < argc) {
            file = argv[++i];
        } else if (arg == "--entry" && i + 1 < argc) {
            options.entry = argv[++i];
        } else if (arg == "--seed" && i + 1 < argc) {
            options.seed = std::strtoull(argv[++i], nullptr, 10);
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
    const auto result = interpreter::compile(*text.text, file.filename().string(), options);
    if (!result.ok()) {
        for (const auto& e : result.errors) std::cerr << file.parent_path().string() << (file.has_parent_path() ? "/" : "") << e.text() << '\n';
        return 1;
    }
    std::cout << ir::write(*result.graph, 2) << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--compile") return compileFile(argc, argv);
    }
    tui::appOptions options;
    options.link.clientVersion = PROJ_VERSION;
    bool haveRoot = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            return 0;
        }
        if (arg == "--version") {
            std::cout << "proj-client " << PROJ_VERSION << '\n';
            return 0;
        }
        if (arg == "--settings") {
            if (++i >= argc) {
                usage(std::cerr);
                return 2;
            }
            options.settingsFile = argv[i];
            continue;
        }
        if (arg == "--port") {
            const long port = i + 1 < argc ? std::strtol(argv[++i], nullptr, 10) : 0;
            if (port <= 0 || port > 65535) {
                std::cerr << "proj-client: --port needs a number from 1 to 65535\n";
                return 2;
            }
            options.link.server.serverPort = static_cast<std::uint16_t>(port);
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "proj-client: unknown option " << arg << '\n';
            usage(std::cerr);
            return 2;
        }
        if (!haveRoot) {
            options.projectRoot = argv[i];
            haveRoot = true;
        } else {
            options.files.emplace_back(argv[i]);
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
