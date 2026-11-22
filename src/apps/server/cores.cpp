// The processor cores this process may run on (threads.md §7.1).

#include "cores.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include "../arguments.h"

namespace server {

namespace {

// The first line of @p path, or empty.
std::string firstLine(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

#if defined(__linux__)
// The CPUs in this process's affinity mask; empty when the call fails.
std::set<int> allowedCpus() {
    cpu_set_t mask{};
    std::set<int> cpus;
    if (sched_getaffinity(0, sizeof mask, &mask) != 0) return cpus;
    for (std::size_t cpu = 0; cpu < static_cast<std::size_t>(CPU_SETSIZE); ++cpu) {
        if (CPU_ISSET(cpu, &mask)) cpus.insert(static_cast<int>(cpu));
    }
    return cpus;
}
#endif

}  // namespace

std::vector<cpuPlace> cpusUnder(const std::filesystem::path& root) {
    std::vector<cpuPlace> cpus;
    std::error_code error;
    for (const auto& e : std::filesystem::directory_iterator(root, error)) {
        const auto name = e.path().filename().string();
        // cpu0, cpu1, ...; not cpufreq or cpuidle.
        const auto cpu =
            name.starts_with("cpu") ? apps::parseNumber<int>(std::string_view(name).substr(3)) : std::nullopt;
        const auto topology = e.path() / "topology";
        const auto core = apps::parseNumber<int>(firstLine(topology / "core_id"));
        if (!cpu || !core) continue;
        cpus.push_back(
            {.cpu = *cpu,
             .core = *core,
             .package = apps::parseNumber<int>(firstLine(topology / "physical_package_id")).value_or(0)});
    }
    return cpus;
}

int coresAmong(std::span<const cpuPlace> cpus, const std::set<int>& allowed) {
    // The hardware threads of one core share its (package, core) pair.
    std::set<std::pair<int, int>> cores;
    for (const auto& c : cpus) {
        if (allowed.contains(c.cpu)) cores.emplace(c.package, c.core);
    }
    return static_cast<int>(cores.size());
}

int physicalCores() {
#if defined(__APPLE__)
    int cores = 0;
    std::size_t size = sizeof cores;
    if (sysctlbyname("hw.physicalcpu", &cores, &size, nullptr, 0) == 0 && cores > 0) return cores;
#elif defined(__linux__)
    if (const int cores = coresAmong(cpusUnder("/sys/devices/system/cpu"), allowedCpus()); cores > 0)
        return cores;
#endif
    return usableProcessors();
}

int usableProcessors() {
#if defined(__linux__)
    if (const auto cpus = allowedCpus(); !cpus.empty()) return static_cast<int>(cpus.size());
#endif
    return std::max(1, static_cast<int>(std::thread::hardware_concurrency()));  // the hardware threads
}

int defaultThreads() { return std::clamp(physicalCores(), 1, 16); }

}  // namespace server
