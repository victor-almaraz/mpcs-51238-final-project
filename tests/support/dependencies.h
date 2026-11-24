// Checks a dependency list against the conflicts of its nodes, found from
// each resource's users (threads.md §4.1, §9.2), and draws random orders
// of nodes to check.

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/io.h>
#include <espresso/primitives/pipe.h>
#include <espresso/primitives/plan.h>

namespace support {

/// True when a read of @p r reads the block before, and so conflicts with
/// no write: an audio or control pipe in @p feedback, sorted
/// (`primitives.md` §5.6.2).
inline bool delayed(metadata::pipeRef r, std::span<const metadata::pipeRef> feedback) {
    return (r.kind == metadata::pipeKind::audio || r.kind == metadata::pipeKind::control) &&
           std::ranges::binary_search(feedback, r);
}

/// The private pipe that @p n writes in place of @p w (threads.md §4.2),
/// or none when the write goes to @p w itself.
inline std::shared_ptr<const primitives::aPipe> privateWriteOf(const primitives::nodeInfo& n,
                                                               metadata::pipeRef w) {
    const auto p = std::ranges::find(n.privateWrites, w, &primitives::nodeInfo::privateWrite::shared);
    return p == n.privateWrites.end() ? nullptr : p->pipe;
}

/// Every pair of nodes, earlier first, that use one resource where at
/// least one writes it, apart from delayed() reads. Found from each
/// resource's whole list of users, not from its last writer as the builder
/// does.
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
    std::vector<use> faults;
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        const primitives::nodeInfo& n = nodes[i];
        for (const auto r : n.reads) {
            if (!delayed(r, feedback)) pipes[r].push_back({.node = i, .write = false});
        }
        for (const auto w : n.writes) {
            // A write to a private pipe uses only that pipe.
            const auto p = privateWriteOf(n, w);
            (p ? privates[p] : pipes[w]).push_back({.node = i, .write = true});
        }
        for (const auto& p : n.privateReads) privates[p].push_back({.node = i, .write = false});
        for (const auto& b : n.bufferReads) buffers[b].push_back({.node = i, .write = false});
        for (const auto& b : n.bufferWrites) buffers[b].push_back({.node = i, .write = true});
        for (const auto& p : n.ports) ports[p].push_back({.node = i, .write = true});
        if (n.writesDevice) device.push_back({.node = i, .write = true});
        if (n.reportsFaults) faults.push_back({.node = i, .write = true});
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
    pairs(faults);
    return out;
}

/// Which nodes of a dependency list each node follows, directly or through
/// others: a bit for each pair, row by row in one flat matrix.
class precedence {
public:
    /// The precedence of @p deps; fails the test unless each node depends
    /// only on earlier nodes.
    explicit precedence(const primitives::dependencyList& deps)
        : words_((deps.size() + 63) / 64), bits_(deps.size() * words_, 0) {
        for (std::size_t j = 0; j < deps.size(); ++j) {
            for (const std::uint32_t i : deps.of(j)) {
                REQUIRE(i < j);
                row(j)[i / 64] |= std::uint64_t{1} << (i % 64);
                for (std::size_t w = 0; w < words_; ++w) row(j)[w] |= row(i)[w];
            }
        }
    }

    /// True when node @p j follows node @p i.
    bool operator()(std::size_t j, std::size_t i) const noexcept {
        return (bits_[(j * words_) + (i / 64)] >> (i % 64) & 1U) != 0;
    }

private:
    std::span<std::uint64_t> row(std::size_t j) { return std::span(bits_).subspan(j * words_, words_); }

    std::size_t words_ = 0;
    std::vector<std::uint64_t> bits_;
};

/// Which nodes each node of @p deps follows: `follows(deps)(j, i)` is true
/// when node j runs after node i in every schedule that keeps @p deps.
inline precedence follows(const primitives::dependencyList& deps) { return precedence(deps); }

/// Checks that @p deps, for @p nodes in their order, lists only earlier
/// nodes in conflict with each node, sorted, and that every node in
/// conflict with an earlier one follows it through the list. Returns the
/// number of conflicting pairs.
inline std::size_t
checkDependencies(std::span<const std::reference_wrapper<const primitives::nodeInfo>> nodes,
                  std::span<const metadata::pipeRef> feedback, const primitives::dependencyList& deps) {
    REQUIRE(deps.size() == nodes.size());
    const auto conflicting = conflicts(nodes, feedback);
    const auto after = follows(deps);
    std::size_t unsorted = 0;
    std::size_t needless = 0;  // listed without a conflict
    for (std::size_t j = 0; j < deps.size(); ++j) {
        const auto list = deps.of(j);
        if (!std::ranges::is_sorted(list) || std::ranges::adjacent_find(list) != list.end()) ++unsorted;
        for (const std::uint32_t i : list) {
            if (!conflicting.contains({i, static_cast<std::uint32_t>(j)})) ++needless;
        }
    }
    std::size_t unordered = 0;
    for (const auto& [i, j] : conflicting) {
        if (!after(j, i)) {
            UNSCOPED_INFO("node " << j << " conflicts with node " << i << " but need not follow it");
            ++unordered;
        }
    }
    CHECK(unsorted == 0);
    CHECK(needless == 0);
    CHECK(unordered == 0);
    return conflicting.size();
}

/// What random orders use besides table pipes: three each of buffers, I/O
/// ports, and private pipes.
struct orderResources {
    orderResources() {
        for (int i = 0; i < 3; ++i) {
            buffers.push_back(std::make_shared<primitives::sampleBuffer>(1, 16, 48000.0));
            ports.push_back(hub.openMidiOut("device " + std::to_string(i)));
            privates.push_back(std::make_shared<primitives::aPipe>(16));
        }
    }

    primitives::ioHub hub;
    std::vector<std::shared_ptr<const primitives::sampleBuffer>> buffers;
    std::vector<std::shared_ptr<const primitives::ioPort>> ports;
    std::vector<std::shared_ptr<primitives::aPipe>> privates;
};

/// An order of nodes, its feedback pipes, and the pipes it uses besides
/// its nodes' (a plan's `used`), each list sorted and unique.
struct nodeOrder {
    std::vector<primitives::nodeInfo> nodes;
    std::vector<metadata::pipeRef> feedback;
    std::vector<metadata::pipeRef> used;

    /// The nodes, in their order, as the builder's analysis takes them.
    std::vector<std::reference_wrapper<const primitives::nodeInfo>> order() const {
        return {nodes.begin(), nodes.end()};
    }
};

/// A random order of 1 to 40 nodes drawn with @p random, as randomDeps()
/// draws dependency lists: over six pipes of each of the audio, control,
/// and event kinds, of which up to four are feedback and up to two used,
/// and over @p with's resources. Each node reads up to three pipes and
/// writes up to two, sometimes one of them into a private pipe, which
/// others read; some use a buffer, a port, the device's outputs, or the
/// fault queue.
inline nodeOrder randomOrder(const orderResources& with, std::mt19937& random) {
    auto below = [&](int n) { return std::uniform_int_distribution<int>(0, n - 1)(random); };
    auto anyPipe = [&] {
        const auto kind = std::array{metadata::pipeKind::audio, metadata::pipeKind::control,
                                     metadata::pipeKind::event}[static_cast<std::size_t>(below(3))];
        return metadata::pipeRef{.kind = kind, .index = below(6)};
    };
    auto pipes = [&](int draws) {
        std::vector<metadata::pipeRef> out;
        out.reserve(static_cast<std::size_t>(draws));
        for (int i = 0; i < draws; ++i) out.push_back(anyPipe());
        std::ranges::sort(out);
        out.erase(std::ranges::unique(out).begin(), out.end());
        return out;
    };
    auto any = [&](const auto& resources) { return resources[static_cast<std::size_t>(below(3))]; };
    nodeOrder o;
    o.feedback = pipes(4);
    o.used = pipes(2);
    o.nodes.resize(1 + static_cast<std::size_t>(below(40)));
    for (auto& n : o.nodes) {
        for (int k = below(4); k > 0; --k) n.reads.push_back(anyPipe());
        for (int k = below(3); k > 0; --k) n.writes.push_back(anyPipe());
        if (below(6) == 0) n.bufferReads.push_back(any(with.buffers));
        if (below(8) == 0) n.bufferWrites.push_back(any(with.buffers));
        if (below(8) == 0) n.ports.push_back(any(with.ports));
        n.writesDevice = below(10) == 0;
        n.reportsFaults = below(10) == 0;
        if (!n.writes.empty() && below(4) == 0)
            n.privateWrites.push_back({.shared = n.writes.front(), .pipe = any(with.privates)});
        if (below(6) == 0) n.privateReads.push_back(any(with.privates));
    }
    return o;
}

}  // namespace support
