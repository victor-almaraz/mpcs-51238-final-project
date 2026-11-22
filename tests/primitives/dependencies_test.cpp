// Dependencies between the nodes of an order (threads.md §4.1), the
// property test of §9.2 on random graphs, and the work on pipes that the
// dependencies place in the nodes (§6.8).

#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
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
    return primitives::analysisOf(order, feedback).dependencies;
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

TEST_CASE("nodes that report faults follow each other and nothing else") {
    // 0 and 2 report faults; 1, between them, shares nothing with either.
    std::vector<nodeInfo> nodes(4);
    nodes[0].reportsFaults = true;
    nodes[2].reportsFaults = true;
    nodes[3].writesDevice = true;  // a device writer that reports nothing
    const auto d = of(nodes);
    CHECK(listed(d, 1).empty());
    CHECK(listed(d, 2) == std::vector<std::uint32_t>{0});
    CHECK(listed(d, 3).empty());
}

TEST_CASE("for random graphs, the dependencies order every conflicting pair and list only conflicts") {
    const support::orderResources resources;
    std::mt19937 random(Catch::getSeed());  // --rng-seed repeats a run
    std::size_t conflicts = 0;
    for (int graph = 0; graph < 300; ++graph) {
        const auto g = support::randomOrder(resources, random);
        INFO("graph " << graph);
        const auto order = g.order();
        conflicts += support::checkDependencies(order, g.feedback,
                                                primitives::analysisOf(order, g.feedback).dependencies);
    }
    CHECK(conflicts > 1000);  // the graphs are dense enough to test something
}

namespace {

template <typename T>
std::vector<T> listed(const primitives::perNode<T>& lists, std::size_t i) {
    const auto l = lists.of(i);
    return {l.begin(), l.end()};
}

}  // namespace

TEST_CASE("a pipe starts in the node that first uses and writes it and publishes in its last writer "
          "(threads.md 6.8)") {
    const auto mine = std::make_shared<primitives::aPipe>(16);   // written by node 2
    const auto other = std::make_shared<primitives::aPipe>(16);  // no node's: an adopted writer's
    std::vector<nodeInfo> nodes{
        node({}, {audio(0), events(0)}),  // 0: first and last writer of both
        node({audio(0)}, {audio(1)}),     // 1: first writer of audio 1
        node({}, {audio(1)}),             // 2: writes audio 1 into a private pipe
        node({}, {audio(1)}),             // 3: sums it, the last writer of audio 1
        node({audio(2)}, {}),             // 4: reads audio 2 before its writer, as a cut cycle does
        node({}, {audio(2)}),             // 5
        node({audio(3)}, {audio(3)}),     // 6: a feedback loop through audio 3
        node({audio(4), events(1)}, {}),  // 7: reads pipes no node writes
        node({}, {control(0), control(1)}),
    };
    nodes[2].privateWrites = {{.shared = audio(1), .pipe = mine}};
    nodes[3].privateReads = {mine};
    const std::vector<pipeRef> feedback{audio(3), control(1)};
    // Audio 5 is declared and unused; the video pipe has no block work.
    const std::vector<pipeRef> used{audio(5), {.kind = pipeKind::video, .index = 0}};
    const std::vector<std::shared_ptr<primitives::aPipe>> privates{mine, other};
    const std::vector<std::reference_wrapper<const nodeInfo>> order(nodes.begin(), nodes.end());
    const auto w = primitives::analysisOf(order, feedback, used, privates).pipes;

    REQUIRE(w.starts.size() == nodes.size());
    REQUIRE(w.privateStarts.size() == nodes.size());
    REQUIRE(w.publishes.size() == nodes.size());
    CHECK(listed(w.starts, 0) == std::vector<pipeRef>{audio(0), events(0)});
    CHECK(listed(w.starts, 1) == std::vector<pipeRef>{audio(1)});
    for (std::size_t i = 2; i < nodes.size(); ++i) CHECK(listed(w.starts, i).empty());
    CHECK(listed(w.privateStarts, 2) == std::vector<std::shared_ptr<primitives::aPipe>>{mine});
    CHECK(listed(w.publishes, 0) == std::vector<pipeRef>{audio(0)});
    CHECK(listed(w.publishes, 1).empty());  // a later node writes audio 1
    CHECK(listed(w.publishes, 3) == std::vector<pipeRef>{audio(1)});
    CHECK(listed(w.publishes, 5) == std::vector<pipeRef>{audio(2)});
    CHECK(w.serialStarts == std::vector<pipeRef>{audio(2), audio(3), audio(4), audio(5), events(1)});
    CHECK(w.serialPrivateStarts == std::vector<std::shared_ptr<primitives::aPipe>>{other});
    // Feedback pipes, and audio pipes no node writes; control 0 has nothing to end.
    CHECK(w.serialEnds == std::vector<pipeRef>{audio(3), audio(4), audio(5), control(1)});
}

TEST_CASE("a node's pipe work marks the starts of private pipes with a target apart from the rest "
          "(threads.md 4.2.5)") {
    primitives::aPipe table(16);  // the shared pipe two groups' pipes forward to
    const auto forwards = std::make_shared<primitives::aPipe>(16, 1, false);
    const auto alsoForwards = std::make_shared<primitives::aPipe>(16, 1, false);
    const auto stays = std::make_shared<primitives::aPipe>(16, 1, false);  // no target
    forwards->forwardTo(table);
    alsoForwards->forwardTo(table);
    std::vector<nodeInfo> nodes{
        node({}, {audio(0)}),            // 0: first writer of audio 0, which it starts
        node({}, {audio(0)}),            // 1: writes a private pipe with a target
        node({}, {audio(0)}),            // 2: writes a private pipe without one
        node({}, {audio(0), audio(1)}),  // 3: a private pipe with a target, and starts and publishes audio 1
        node({}, {audio(0)}),            // 4: sums them, the last writer of audio 0, which it publishes
        node({audio(0), audio(1)}, {}),  // 5: reads, with nothing to start or publish
    };
    nodes[1].privateWrites = {{.shared = audio(0), .pipe = forwards}};
    nodes[2].privateWrites = {{.shared = audio(0), .pipe = stays}};
    nodes[3].privateWrites = {{.shared = audio(0), .pipe = alsoForwards}};
    nodes[4].privateReads = {forwards, stays, alsoForwards};
    const std::vector<std::reference_wrapper<const nodeInfo>> order(nodes.begin(), nodes.end());
    const auto w = primitives::analysisOf(order, {}).pipes;

    using primitives::pipeWork;
    constexpr auto both = static_cast<std::uint8_t>(pipeWork::touchesPipes | pipeWork::touchesForwarding);
    CHECK(w.touches == std::vector<std::uint8_t>{pipeWork::touchesPipes, pipeWork::touchesForwarding,
                                                 pipeWork::touchesPipes, both, pipeWork::touchesPipes, 0});
}

TEST_CASE("for random graphs each pipe starts once before every use but feedback reads and ends once") {
    const support::orderResources resources;
    const auto& privates = resources.privates;
    std::mt19937 random(Catch::getSeed());  // --rng-seed repeats a run
    std::size_t inNodes = 0;    // starts that fell to a node
    std::size_t published = 0;  // publishes that fell to a node
    for (int graph = 0; graph < 300; ++graph) {
        const auto g = support::randomOrder(resources, random);
        const auto& nodes = g.nodes;
        INFO("graph " << graph);
        const auto order = g.order();
        const auto analysis = primitives::analysisOf(order, g.feedback, g.used, privates);
        support::checkDependencies(order, g.feedback, analysis.dependencies);
        const auto& w = analysis.pipes;
        const std::size_t n = nodes.size();
        REQUIRE(w.starts.size() == n);
        REQUIRE(w.privateStarts.size() == n);
        REQUIRE(w.publishes.size() == n);

        const auto after = support::follows(analysis.dependencies);
        const auto delayed = [&](pipeRef r) { return support::delayed(r, g.feedback); };
        const auto writes = [&](std::size_t i, pipeRef r) {
            return std::ranges::find(nodes[i].writes, r) != nodes[i].writes.end() &&
                   !support::privateWriteOf(nodes[i], r);
        };
        const auto uses = [&](std::size_t i, pipeRef r) {
            return writes(i, r) ||
                   (!delayed(r) && std::ranges::find(nodes[i].reads, r) != nodes[i].reads.end());
        };
        const auto writesOwn = [&](std::size_t i, const std::shared_ptr<primitives::aPipe>& p) {
            return std::ranges::any_of(nodes[i].privateWrites, [&](const auto& x) {
                return x.pipe == p && std::ranges::find(nodes[i].writes, x.shared) != nodes[i].writes.end();
            });
        };
        const auto usesOwn = [&](std::size_t i, const std::shared_ptr<primitives::aPipe>& p) {
            return writesOwn(i, p) ||
                   std::ranges::find(nodes[i].privateReads, p) != nodes[i].privateReads.end();
        };
        // A pipe started by node s, which uses it first and writes it, and
        // which every later user follows.
        std::size_t misplaced = 0;
        const auto checkStarter = [&](std::size_t s, auto usesIt, auto writesIt) {
            if (!writesIt(s)) ++misplaced;
            for (std::size_t i = 0; i < n; ++i) {
                if (i < s && usesIt(i)) ++misplaced;
                if (i > s && usesIt(i) && !after(i, s)) ++misplaced;
            }
        };

        // Every audio and event pipe of the table that is used, or that a
        // node reads or writes other than through a private pipe: started
        // once; each audio pipe ends once.
        std::vector<pipeRef> pipes = g.used;
        for (const auto& x : nodes) {
            pipes.insert(pipes.end(), x.reads.begin(), x.reads.end());
            for (const auto r : x.writes) {
                if (!support::privateWriteOf(x, r)) pipes.push_back(r);
            }
        }
        std::ranges::sort(pipes);
        pipes.erase(std::ranges::unique(pipes).begin(), pipes.end());
        std::size_t wrongCounts = 0;
        for (const auto r : pipes) {
            const auto serialStart = std::ranges::count(w.serialStarts, r);
            const auto serialEnd = std::ranges::count(w.serialEnds, r);
            std::vector<std::size_t> starters;
            std::vector<std::size_t> publishers;
            for (std::size_t i = 0; i < n; ++i) {
                if (std::ranges::count(w.starts.of(i), r) > 0) starters.push_back(i);
                if (std::ranges::count(w.publishes.of(i), r) > 0) publishers.push_back(i);
            }
            if (r.kind == pipeKind::control) {
                if (serialStart + std::ssize(starters) + std::ssize(publishers) != 0) ++wrongCounts;
                if (serialEnd != (std::ranges::binary_search(g.feedback, r) ? 1 : 0)) ++wrongCounts;
                continue;
            }
            if (serialStart + std::ssize(starters) != 1) ++wrongCounts;
            for (const auto s : starters) {
                if (r.kind == pipeKind::audio && delayed(r)) ++misplaced;  // feedback starts serially
                checkStarter(
                    s, [&](std::size_t i) { return uses(i, r); },
                    [&](std::size_t i) { return writes(i, r); });
            }
            inNodes += starters.size();
            if (r.kind == pipeKind::event) {
                if (serialEnd + std::ssize(publishers) != 0) ++wrongCounts;
                continue;
            }
            if (serialEnd + std::ssize(publishers) != 1) ++wrongCounts;
            for (const auto l : publishers) {
                if (delayed(r) || !writes(l, r)) ++misplaced;
                for (std::size_t i = l + 1; i < n; ++i) {
                    if (writes(i, r)) ++misplaced;  // the last writer publishes
                }
            }
            published += publishers.size();
        }
        for (const auto& p : privates) {
            const auto serial = std::ranges::count(w.serialPrivateStarts, p);
            std::vector<std::size_t> starters;
            for (std::size_t i = 0; i < n; ++i) {
                if (std::ranges::count(w.privateStarts.of(i), p) > 0) starters.push_back(i);
            }
            if (serial + std::ssize(starters) != 1) ++wrongCounts;
            for (const auto s : starters) {
                checkStarter(
                    s, [&](std::size_t i) { return usesOwn(i, p); },
                    [&](std::size_t i) { return writesOwn(i, p); });
            }
            inNodes += starters.size();
        }
        // Nothing else: no video pipe, no pipe twice.
        std::size_t items = w.serialStarts.size() + w.serialPrivateStarts.size() + w.starts.items.size() +
                            w.privateStarts.items.size();
        std::size_t expected = privates.size();
        for (const auto r : pipes) {
            if (r.kind == pipeKind::audio || r.kind == pipeKind::event) ++expected;
        }
        CHECK(items == expected);
        CHECK(wrongCounts == 0);
        CHECK(misplaced == 0);
    }
    CHECK(inNodes > 1000);  // the graphs start many pipes in their nodes
    CHECK(published > 500);
}
