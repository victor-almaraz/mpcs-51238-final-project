// Client entry point: TUI editor and interpreter.
//
// Usage: proj-client [--settings FILE] [PROJECT_DIR [FILE...]]

#include <filesystem>
#include <iostream>
#include <string_view>

#include <proj/tui/app.h>

namespace {

void usage(std::ostream& out) {
    out << "usage: proj-client [--settings FILE] [PROJECT_DIR [FILE...]]\n"
           "\n"
           "Opens the editor on PROJECT_DIR (default: the current directory).\n"
           "FILEs are opened relative to PROJECT_DIR.\n";
}

} // namespace

int main(int argc, char** argv) {
    tui::appOptions options;
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
