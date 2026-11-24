// Client entry point: TUI editor and interpreter.
//
// Usage: espresso-client [--settings FILE] [--port N] [PROJECT_DIR [FILE...]]
//        espresso-client --compile FILE [--root DIR] [--entry NAME] [--seed N]

#include <iostream>
#include <utility>
#include <variant>

#include <espresso/tui/app.h>

#include "../arguments.h"
#include "commandLine.h"

// The command line runs in client::run (commandLine.h); the editor's
// full-screen loop, which needs a terminal, runs here.
int main(int argc, char** argv) {
    auto outcome = client::run(apps::arguments(argc, argv), std::cout, std::cerr);
    if (std::holds_alternative<int>(outcome)) return std::get<int>(outcome);
    tui::app editor(std::move(std::get<tui::appOptions>(outcome)));
    return editor.run();
}
