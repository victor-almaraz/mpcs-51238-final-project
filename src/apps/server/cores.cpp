// The machine's processor cores (threads.md §7.1).

#include "cores.h"

#include <algorithm>
#include <cstddef>
#include <thread>

#if defined(__linux__)
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace server {

namespace {

// The hardware threads, at least 1.
int hardwareThreads() { return std::max(1, static_cast<int>(std::thread::hardware_concurrency())); }

#if defined(__linux__)
// The first line of @p path, or empty.
std::string firstLine(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}
#endif

}  // namespace

int physicalCores() {
#if defined(__APPLE__)
    int cores = 0;
    std::size_t size = sizeof cores;
    if (sysctlbyname("hw.physicalcpu", &cores, &size, nullptr, 0) == 0 && cores > 0) return cores;
#elif defined(__linux__)
    // Each core is a (package, core) pair; its hardware threads share it.
    std::set<std::pair<std::string, std::string>> cores;
    std::error_code error;
    for (const auto& e : std::filesystem::directory_iterator("/sys/devices/system/cpu", error)) {
        const auto name = e.path().filename().string();
        if (!name.starts_with("cpu") || name.size() == 3 ||
            !std::isdigit(static_cast<unsigned char>(name[3])))
            continue;
        const auto topology = e.path() / "topology";
        const auto core = firstLine(topology / "core_id");
        if (!core.empty()) cores.emplace(firstLine(topology / "physical_package_id"), core);
    }
    if (!cores.empty()) return static_cast<int>(cores.size());
#endif
    return hardwareThreads();
}

int defaultThreads() { return std::clamp(physicalCores(), 1, 16); }

}  // namespace server
