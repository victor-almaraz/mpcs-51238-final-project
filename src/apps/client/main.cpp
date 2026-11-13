// Client entry point: TUI editor and interpreter.

#include <iostream>

#include <proj/interpreter/module.h>
#include <proj/protocol/module.h>
#include <proj/tui/module.h>

int main() {
    std::cout << "proj-client " << PROJ_VERSION << '\n';
    return 0;
}
