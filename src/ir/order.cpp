// Execution order: writers before readers (AGENTS.md: the compiler sorts
// the graph; the engine runs nodes in IR order).

#include <proj/ir/ir.h>

#include <algorithm>
#include <map>
#include <queue>
#include <set>

namespace ir {

using metadata::pipeRef;

std::vector<std::size_t> sortByDataFlow(metadata::graphSpec& graph) {
    auto& nodes = graph.nodes;
    const std::size_t n = nodes.size();
    std::set<pipeRef> feedback;
    for (const auto& d : graph.pipes) {
        if (d.feedback) feedback.insert(d.pipe);
    }
    // A node reading a pipe it writes is a cycle too.
    std::map<pipeRef, std::vector<std::size_t>> writers;
    for (std::size_t i = 0; i < n; ++i) {
        for (auto r : metadata::pipesWritten(nodes[i])) {
            if (!feedback.count(r)) writers[r].push_back(i);
        }
    }
    std::vector<std::vector<std::size_t>> successors(n);
    std::vector<std::size_t> indegree(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        for (auto r : metadata::pipesRead(nodes[i])) {
            const auto w = writers.find(r);
            if (w == writers.end()) continue;
            for (std::size_t from : w->second) {
                successors[from].push_back(i);
                ++indegree[i];
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
