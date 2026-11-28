// Client entry point: TUI editor and interpreter.
//
// Usage: espresso-client [--settings FILE] [--port N] [PROJECT_DIR [FILE...]]
//        espresso-client --compile FILE [--root DIR] [--entry NAME] [--seed N]
//        espresso-client --page FILE [--root DIR] [--entry NAME] [--seed N]
//        espresso-client --serve [--port N]

#include <iostream>
#include <utility>
#include <variant>

#include <espresso/tui/app.h>

#include "../arguments.h"
#include "commandLine.h"
#include "serve.h"

// The command line runs in client::run (commandLine.h); the editor's
// full-screen loop, which needs a terminal, and the sidecar, which needs
// the process's standard input and output, run here.
int main(int argc, char** argv) {
    auto outcome = client::run(apps::arguments(argc, argv), std::cout, std::cerr);
    if (std::holds_alternative<int>(outcome)) return std::get<int>(outcome);
    if (std::holds_alternative<client::serveOptions>(outcome)) {
        return client::serve(std::cin, std::cout, std::move(std::get<client::serveOptions>(outcome)));
    }
    tui::app editor(std::move(std::get<tui::appOptions>(outcome)));
    return editor.run();
}
