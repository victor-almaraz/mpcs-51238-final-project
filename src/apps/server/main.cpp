// Server entry point: audio core, devices, and the visual output window.

#include <iostream>

#include <juce_core/juce_core.h>

#include <proj/ir/module.h>
#include <proj/primitives/module.h>

int main() {
    std::cout << "proj-server " << PROJ_VERSION << " ("
              << juce::SystemStats::getJUCEVersion() << ")\n";
    return 0;
}
