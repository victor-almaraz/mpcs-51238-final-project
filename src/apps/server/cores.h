// The processor cores this process may run on, for the default thread
// count (threads.md §7.1). Has no JUCE dependency.

#pragma once

#include <filesystem>
#include <set>
#include <span>
#include <vector>

namespace server {

/// A logical CPU and the physical core and package that hold it, as
/// Linux's `/sys/devices/system/cpu/cpuN/topology` gives them.
struct cpuPlace {
    int cpu = 0;      ///< The CPU's number, N in `cpuN`
    int core = 0;     ///< `topology/core_id`, unique within its package
    int package = 0;  ///< `topology/physical_package_id`
};

/// The CPUs under @p root, a directory laid out as Linux's
/// `/sys/devices/system/cpu`: each `cpuN` with a readable
/// `topology/core_id`, in no particular order. An unreadable
/// `physical_package_id` reads as package 0.
std::vector<cpuPlace> cpusUnder(const std::filesystem::path& root);

/// The physical cores, distinct (package, core) pairs, that hold at least
/// one CPU of @p cpus whose number is in @p allowed.
int coresAmong(std::span<const cpuPlace> cpus, const std::set<int>& allowed);

/// The physical cores that hold a CPU this process may run on, at least
/// 1: `hw.physicalcpu` on macOS; on Linux, the cores in
/// `/sys/devices/system/cpu` of the CPUs in the process's affinity mask,
/// which `taskset`, cgroup cpusets, and `isolcpus` narrow. When neither
/// answers, the usable processors.
int physicalCores();

/// The logical processors this process may run on, at least 1: the CPUs
/// in its affinity mask on Linux, the hardware threads elsewhere.
int usableProcessors();

/// The default of `--threads` (threads.md §7.1, proposed): the physical
/// cores, at most 16.
int defaultThreads();

}  // namespace server
