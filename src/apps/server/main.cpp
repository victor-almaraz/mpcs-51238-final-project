// Server entry point: audio core, devices, and the visual output window.

#include <iostream>

#include <juce_core/juce_core.h>

#include <proj/metadata/version.h>

int main() {
    std::cout << "proj-server " << PROJ_VERSION << " (" << juce::SystemStats::getJUCEVersion()
              << ", metadata " << metadata::version << ")\n";
    return 0;
}
