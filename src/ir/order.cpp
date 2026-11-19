// Execution order: writers before readers (AGENTS.md: the compiler sorts
// the graph; the engine runs nodes in IR order).

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <queue>
#include <utility>
#include <vector>

#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>
#include <espresso/metadata/primitiveSpec.h>

namespace ir {

namespace {

// True when @p to can be reached from @p from along @p successors.
bool reaches(const std::vector<std::vector<std::size_t>>& successors, std::size_t from, std::size_t to) {
    std::vector<bool> seen(successors.size(), false);
    std::vector<std::size_t> stack{from};
    while (!stack.empty()) {
        const std::size_t i = stack.back();
        stack.pop_back();
        if (i == to) return true;
        if (seen[i]) continue;
        seen[i] = true;
        stack.insert(stack.end(), successors[i].begin(), successors[i].end());
    }
    return false;
}

}  // namespace

std::vector<std::size_t> sortByDataFlow(metadata::graphSpec& graph) {
    auto& nodes = graph.nodes;
    const std::size_t n = nodes.size();
    // A node reading a pipe it writes is a cycle too.
    const auto writers = metadata::writersByPipe(graph, true);
    std::vector<std::vector<std::size_t>> successors(n);
    std::vector<std::size_t> indegree(n, 0);
    const auto edge = [&](std::size_t from, std::size_t to) {
        successors[from].push_back(to);
        ++indegree[to];
    };
    for (std::size_t i = 0; i < n; ++i) {
        for (auto r : metadata::pipesRead(nodes[i])) {
            const auto w = writers.find(r);
            if (w == writers.end()) continue;
            for (std::size_t from : w->second) edge(from, i);
        }
    }
    // A buffer's writers run before its readers, unless the edge would
    // close a cycle; then the rest of the graph decides (samples.md §5.7).
    std::map<metadata::bufferRef, std::vector<std::size_t>> bufferWriters;
    for (std::size_t i = 0; i < n; ++i) {
        for (const auto b : metadata::buffersOf(nodes[i]).writes) bufferWriters[b].push_back(i);
    }
    for (std::size_t i = 0; i < n; ++i) {
        for (const auto b : metadata::buffersOf(nodes[i]).reads) {
            const auto w = bufferWriters.find(b);
            if (w == bufferWriters.end()) continue;
            for (std::size_t from : w->second) {
                if (from != i && !reaches(successors, i, from)) edge(from, i);
            }
        }
    }
    // Ties keep the original order.
    std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
    for (std::size_t i = 0; i < n; ++i) {
        if (indegree[i] == 0) ready.push(i);
    }
    std::vector<std::size_t> order;
    while (!ready.empty()) {
        const std::size_t i = ready.top();
        ready.pop();
        order.push_back(i);
        for (std::size_t s : successors[i]) {
            if (--indegree[s] == 0) ready.push(s);
        }
    }
    if (order.size() < n) {
        // The nodes left are on a cycle or downstream of one. Peel off those
        // with no remaining successor until only the cycles are left.
        std::vector<bool> left(n, false);
        for (std::size_t i = 0; i < n; ++i) left[i] = indegree[i] != 0;
        for (bool changed = true; changed;) {
            changed = false;
            for (std::size_t i = 0; i < n; ++i) {
                if (left[i] && std::none_of(successors[i].begin(), successors[i].end(), [&](std::size_t s) { return left[s]; })) {
                    left[i] = false;
                    changed = true;
                }
            }
        }
        std::vector<std::size_t> cycle;
        for (std::size_t i = 0; i < n; ++i) {
            if (left[i]) cycle.push_back(i);
        }
        return cycle;
    }
    std::vector<metadata::nodeSpec> sorted;
    sorted.reserve(n);
    for (std::size_t i : order) sorted.push_back(std::move(nodes[i]));
    nodes = std::move(sorted);
    return {};
}

} // namespace ir
