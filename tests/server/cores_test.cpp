// The physical cores among the CPUs a process may run on, which set the
// default thread count (threads.md §7.1).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <vector>

#include "cores.h"
#include "support/tempDir.h"

using server::coresAmong;
using server::cpuPlace;

namespace {

// @p cores cores of one package with two hardware threads each, numbered
// as Linux numbers them: CPU n and CPU n + cores share core n.
std::vector<cpuPlace> smtPairs(int cores) {
    std::vector<cpuPlace> cpus;
    for (int thread = 0; thread < 2; ++thread) {
        for (int core = 0; core < cores; ++core) cpus.push_back({.cpu = thread * cores + core, .core = core});
    }
    return cpus;
}

}  // namespace

TEST_CASE("cores count two hardware threads of one core once") {
    const auto cpus = smtPairs(4);
    CHECK(coresAmong(cpus, {0, 1, 2, 3, 4, 5, 6, 7}) == 4);
    CHECK(coresAmong(cpus, {1, 5}) == 1);
}

TEST_CASE("a mask that allows one hardware thread of each core counts every core") {
    const auto cpus = smtPairs(4);
    CHECK(coresAmong(cpus, {0, 1, 2, 3}) == 4);
    CHECK(coresAmong(cpus, {4, 1, 6, 3}) == 4);
}

TEST_CASE("a mask of one CPU counts one core") {
    CHECK(coresAmong(smtPairs(4), {6}) == 1);
    CHECK(coresAmong(smtPairs(1), {0}) == 1);
}

TEST_CASE("cores of two packages with the same core IDs count apart") {
    // Two packages of two cores, without hardware threads.
    const std::vector<cpuPlace> cpus{{.cpu = 0, .core = 0, .package = 0},
                                     {.cpu = 1, .core = 1, .package = 0},
                                     {.cpu = 2, .core = 0, .package = 1},
                                     {.cpu = 3, .core = 1, .package = 1}};
    CHECK(coresAmong(cpus, {0, 1, 2, 3}) == 4);
    CHECK(coresAmong(cpus, {0, 2}) == 2);
    CHECK(coresAmong(cpus, {2, 3}) == 2);
}

TEST_CASE("CPUs outside the mask or the topology add no core") {
    const auto cpus = smtPairs(2);
    CHECK(coresAmong(cpus, {}) == 0);
    CHECK(coresAmong(cpus, {9}) == 0);
    CHECK(coresAmong(cpus, {0, 9}) == 1);
}

TEST_CASE("the topology is read from each cpuN directory with a core ID") {
    const support::tempDir root;
    root.write("cpu0/topology/core_id", "0\n");
    root.write("cpu0/topology/physical_package_id", "0\n");
    root.write("cpu1/topology/core_id", "0\n");
    root.write("cpu1/topology/physical_package_id", "1\n");
    root.write("cpu12/topology/core_id", "3\n");  // no package ID: package 0
    root.write("cpu2/online", "0\n");             // no topology directory
    root.write("cpufreq/topology/core_id", "7\n");
    root.write("cpuidle/state", "\n");
    root.write("cpu3x/topology/core_id", "1\n");
    root.write("cpu4/topology/core_id", "unknown\n");
    auto cpus = server::cpusUnder(root.path());
    std::ranges::sort(cpus, {}, &cpuPlace::cpu);
    REQUIRE(cpus.size() == 3);
    CHECK((cpus[0].cpu == 0 && cpus[0].core == 0 && cpus[0].package == 0));
    CHECK((cpus[1].cpu == 1 && cpus[1].core == 0 && cpus[1].package == 1));
    CHECK((cpus[2].cpu == 12 && cpus[2].core == 3 && cpus[2].package == 0));
    CHECK(coresAmong(cpus, {0, 1, 12}) == 3);
}

TEST_CASE("a missing topology directory reads as no CPUs") {
    const support::tempDir root;
    CHECK(server::cpusUnder(root / "missing").empty());
}

TEST_CASE("the default thread count fits the processors this process may use") {
    CHECK(server::physicalCores() >= 1);
    CHECK(server::usableProcessors() >= 1);
    CHECK(server::defaultThreads() >= 1);
    CHECK(server::defaultThreads() <= std::min(16, server::usableProcessors()));
}
