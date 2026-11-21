// Checks a dependency list against the conflicts of its nodes, found from
// each resource's users (threads.md §4.1, §9.2).

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/io.h>
#include <espresso/primitives/pipe.h>
#include <espresso/primitives/plan.h>

namespace support {

/// Every pair of nodes, earlier first, that use one resource where at
/// least one writes it, apart from reads of an audio or control pipe in
/// @p feedback. Found from each resource's whole list of users, not from
/// its last writer as the builder does.
inline std::set<std::pair<std::uint32_t, std::uint32_t>>
conflicts(std::span<const std::reference_wrapper<const primitives::nodeInfo>> nodes,
          std::span<const metadata::pipeRef> feedback) {
    struct use {
        std::uint32_t node;
        bool write;
    };
    std::map<metadata::pipeRef, std::vector<use>> pipes;
    std::map<std::shared_ptr<const primitives::sampleBuffer>, std::vector<use>, std::owner_less<>> buffers;
    std::map<std::shared_ptr<const primitives::ioPort>, std::vector<use>, std::owner_less<>> ports;
    std::map<std::shared_ptr<const primitives::aPipe>, std::vector<use>, std::owner_less<>> privates;
    std::vector<use> device;
    auto delayed = [&](metadata::pipeRef r) {
        return (r.kind == metadata::pipeKind::audio || r.kind == metadata::pipeKind::control) &&
               std::ranges::binary_search(feedback, r);
    };
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        const primitives::nodeInfo& n = nodes[i];
        for (const auto r : n.reads) {
            if (!delayed(r)) pipes[r].push_back({.node = i, .write = false});
        }
        for (const auto w : n.writes) {
            // A write to a private pipe (threads.md §4.2) uses only that pipe.
            const auto p = std::ranges::find(n.privateWrites, w, &primitives::nodeInfo::privateWrite::shared);
            (p == n.privateWrites.end() ? pipes[w] : privates[p->pipe]).push_back({.node = i, .write = true});
        }
        for (const auto& p : n.privateReads) privates[p].push_back({.node = i, .write = false});
        for (const auto& b : n.bufferReads) buffers[b].push_back({.node = i, .write = false});
        for (const auto& b : n.bufferWrites) buffers[b].push_back({.node = i, .write = true});
        for (const auto& p : n.ports) ports[p].push_back({.node = i, .write = true});
        if (n.writesDevice) device.push_back({.node = i, .write = true});
    }
    std::set<std::pair<std::uint32_t, std::uint32_t>> out;
    auto pairs = [&](const std::vector<use>& uses) {
        for (std::size_t x = 0; x < uses.size(); ++x) {
            for (std::size_t y = x + 1; y < uses.size(); ++y) {
                const auto [a, b] = std::minmax(uses[x].node, uses[y].node);
                if (a != b && (uses[x].write || uses[y].write)) out.emplace(a, b);
            }
        }
    };
    for (const auto& [r, uses] : pipes) pairs(uses);
    for (const auto& [b, uses] : buffers) pairs(uses);
    for (const auto& [p, uses] : ports) pairs(uses);
    for (const auto& [p, uses] : privates) pairs(uses);
    pairs(device);
    return out;
}

/// Checks that @p deps, for @p nodes in their order, lists only earlier
/// nodes in conflict with each node, sorted, and that every node in
/// conflict with an earlier one follows it through the list. Returns the
/// number of conflicting pairs.
inline std::size_t
checkDependencies(std::span<const std::reference_wrapper<const primitives::nodeInfo>> nodes,
                  std::span<const metadata::pipeRef> feedback, const primitives::dependencyList& deps) {
    REQUIRE(deps.size() == nodes.size());
    const auto conflicting = conflicts(nodes, feedback);
    const std::size_t n = nodes.size();
    const std::size_t words = (n + 63) / 64;
    // Row j: the nodes j runs after, a bit each, in one flat matrix.
    std::vector<std::uint64_t> matrix(n * words, 0);
    auto follows = [&](std::size_t j) { return std::span(matrix).subspan(j * words, words); };
    std::size_t unsorted = 0;
    std::size_t needless = 0;  // listed without a conflict
    for (std::size_t j = 0; j < n; ++j) {
        const auto list = deps.of(j);
        if (!std::ranges::is_sorted(list) || std::ranges::adjacent_find(list) != list.end()) ++unsorted;
        for (const std::uint32_t i : list) {
            REQUIRE(i < j);
            if (!conflicting.contains({i, static_cast<std::uint32_t>(j)})) ++needless;
            follows(j)[i / 64] |= std::uint64_t{1} << (i % 64);
            for (std::size_t w = 0; w < words; ++w) follows(j)[w] |= follows(i)[w];
        }
    }
    std::size_t unordered = 0;
    for (const auto& [i, j] : conflicting) {
        if ((follows(j)[i / 64] >> (i % 64) & 1U) == 0) {
            UNSCOPED_INFO("node " << j << " conflicts with node " << i << " but need not follow it");
            ++unordered;
        }
    }
    CHECK(unsorted == 0);
    CHECK(needless == 0);
    CHECK(unordered == 0);
    return conflicting.size();
}

}  // namespace support
