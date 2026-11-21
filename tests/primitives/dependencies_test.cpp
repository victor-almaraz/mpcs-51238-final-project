// Dependencies between the nodes of an order (threads.md §4.1), and the
// property test of §9.2 on random graphs.

#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/io.h>
#include <espresso/primitives/pipe.h>
#include <espresso/primitives/plan.h>

#include "../../src/primitives/dependencies.h"
#include "support/dependencies.h"
#include "support/pipes.h"

using metadata::pipeKind;
using metadata::pipeRef;
using primitives::dependencyList;
using primitives::nodeInfo;
using support::audio;
using support::control;
using support::events;

namespace {


nodeInfo node(std::vector<pipeRef> reads, std::vector<pipeRef> writes) {
    nodeInfo n;
    n.reads = std::move(reads);
    n.writes = std::move(writes);
    return n;
}

dependencyList of(const std::vector<nodeInfo>& nodes, std::span<const pipeRef> feedback = {}) {
    const std::vector<std::reference_wrapper<const nodeInfo>> order(nodes.begin(), nodes.end());
    return primitives::dependenciesOf(order, feedback);
}

std::vector<std::uint32_t> listed(const dependencyList& d, std::size_t i) {
    const auto l = d.of(i);
    return {l.begin(), l.end()};
}

}  // namespace

TEST_CASE("a reader follows the last writer, and a writer every reader since") {
    // 0 writes a; 1 and 2 read it; 3 writes it again; 4 reads it.
    const auto d = of({node({}, {audio(0)}), node({audio(0)}, {audio(1)}), node({audio(0)}, {audio(2)}),
                       node({}, {audio(0)}), node({audio(0)}, {})});
    CHECK(listed(d, 0).empty());
    CHECK(listed(d, 1) == std::vector<std::uint32_t>{0});
    CHECK(listed(d, 2) == std::vector<std::uint32_t>{0});  // two readers do not conflict
    CHECK(listed(d, 3) == std::vector<std::uint32_t>{0, 1, 2});
    CHECK(listed(d, 4) == std::vector<std::uint32_t>{3});
}

TEST_CASE("nodes that share nothing, or only read, are independent") {
    const auto d =
        of({node({control(1)}, {audio(0)}), node({control(1)}, {audio(1)}), node({}, {events(0)})});
    for (std::size_t i = 0; i < 3; ++i) CHECK(listed(d, i).empty());
}

TEST_CASE(
    "a read of an audio or control feedback pipe conflicts with no write; an event pipe has one buffer") {
    const std::vector<pipeRef> feedback{audio(0), control(0), events(0)};
    // Each pipe read before it is written, as in a cycle through it.
    const auto d = of({node({audio(0), control(0), events(0)}, {}),
                       node({}, {audio(0), control(0), events(0)}), node({}, {audio(0)})},
                      feedback);
    CHECK(listed(d, 1) == std::vector<std::uint32_t>{0});  // through the event pipe only
    CHECK(listed(d, 2) == std::vector<std::uint32_t>{1});  // writers of one pipe keep their order
}

TEST_CASE("users of one buffer, one port, or the device's outputs follow each other") {
    primitives::ioHub hub;
    const auto port = hub.openMidiOut("synth");
    const auto buffer = std::make_shared<primitives::sampleBuffer>(1, 64, 48000.0);
    std::vector<nodeInfo> nodes(7);
    nodes[0].bufferWrites = {buffer};
    nodes[1].bufferReads = {buffer};
    nodes[2].bufferReads = {buffer};
    nodes[3].ports = {port};
    nodes[4].ports = {port};
    nodes[5].writesDevice = true;
    nodes[6].writesDevice = true;
    const auto d = of(nodes);
    CHECK(listed(d, 1) == std::vector<std::uint32_t>{0});
    CHECK(listed(d, 2) == std::vector<std::uint32_t>{0});
    CHECK(listed(d, 3).empty());
    CHECK(listed(d, 4) == std::vector<std::uint32_t>{3});
    CHECK(listed(d, 6) == std::vector<std::uint32_t>{5});
}

TEST_CASE("for random graphs, the dependencies order every conflicting pair and list only conflicts") {
    primitives::ioHub hub;
    std::vector<std::shared_ptr<const primitives::sampleBuffer>> buffers;
    std::vector<std::shared_ptr<const primitives::ioPort>> ports;
    std::vector<std::shared_ptr<const primitives::aPipe>> privates;
    for (int i = 0; i < 3; ++i) {
        buffers.push_back(std::make_shared<primitives::sampleBuffer>(1, 16, 48000.0));
        ports.push_back(hub.openMidiOut("device " + std::to_string(i)));
        privates.push_back(std::make_shared<primitives::aPipe>(16));
    }
    std::mt19937 random(Catch::getSeed());  // --rng-seed repeats a run
    auto below = [&](int n) { return std::uniform_int_distribution<int>(0, n - 1)(random); };
    auto anyPipe = [&] {
        const auto kind = std::array{pipeKind::audio, pipeKind::control,
                                     pipeKind::event}[static_cast<std::size_t>(below(3))];
        return pipeRef{.kind = kind, .index = below(6)};
    };
    std::size_t conflicts = 0;
    for (int graph = 0; graph < 300; ++graph) {
        std::vector<pipeRef> feedback;
        feedback.reserve(4);
        for (int i = 0; i < 4; ++i) feedback.push_back(anyPipe());
        std::ranges::sort(feedback);
        feedback.erase(std::ranges::unique(feedback).begin(), feedback.end());
        std::vector<nodeInfo> nodes(static_cast<std::size_t>(1 + below(40)));
        for (auto& n : nodes) {
            for (int k = below(4); k > 0; --k) n.reads.push_back(anyPipe());
            for (int k = below(3); k > 0; --k) n.writes.push_back(anyPipe());
            if (below(6) == 0) n.bufferReads.push_back(buffers[static_cast<std::size_t>(below(3))]);
            if (below(8) == 0) n.bufferWrites.push_back(buffers[static_cast<std::size_t>(below(3))]);
            if (below(8) == 0) n.ports.push_back(ports[static_cast<std::size_t>(below(3))]);
            n.writesDevice = below(10) == 0;
            // Some writes go to a private pipe instead (§4.2), which others read.
            if (!n.writes.empty() && below(4) == 0)
                n.privateWrites.push_back(
                    {.shared = n.writes.front(), .pipe = privates[static_cast<std::size_t>(below(3))]});
            if (below(6) == 0) n.privateReads.push_back(privates[static_cast<std::size_t>(below(3))]);
        }
        INFO("graph " << graph);
        const std::vector<std::reference_wrapper<const nodeInfo>> order(nodes.begin(), nodes.end());
        conflicts += support::checkDependencies(order, feedback, primitives::dependenciesOf(order, feedback));
    }
    CHECK(conflicts > 1000);  // the graphs are dense enough to test something
}
