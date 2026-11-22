// Grouping the nodes of an execution order into tasks (threads.md §5).

#include "tasks.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/pipe.h>
#include <espresso/primitives/plan.h>
#include <espresso/primitives/random.h>

#include "costs.h"
#include "dependencies.h"

namespace primitives {

namespace {

// Disjoint sets of indices, each named by one of its members.
class disjointSets {
public:
    explicit disjointSets(std::size_t n) : parent_(n) {
        std::iota(parent_.begin(), parent_.end(), std::uint32_t{0});
    }

    std::uint32_t find(std::uint32_t x) {
        while (parent_[x] != x) {
            parent_[x] = parent_[parent_[x]];
            x = parent_[x];
        }
        return x;
    }
    // Puts @p from's set into @p into's.
    void join(std::uint32_t into, std::uint32_t from) { parent_[find(from)] = find(into); }

private:
    std::vector<std::uint32_t> parent_;
};

// Numbers the sets of @p sets by their first member, in order: the number
// of each of @p n members' sets, and the count of sets.
std::pair<std::vector<std::uint32_t>, std::uint32_t> numbered(disjointSets& sets, std::size_t n) {
    std::vector<std::optional<std::uint32_t>> number(n);
    std::vector<std::uint32_t> of(n);
    std::uint32_t count = 0;
    for (std::uint32_t x = 0; x < n; ++x) {
        auto& id = number[sets.find(x)];
        if (!id) id = count++;
        of[x] = *id;
    }
    return {std::move(of), count};
}

}  // namespace

taskList tasksOf(const dependencyList& deps, std::span<const double> cost, const scheduleOptions& options) {
    const double minimum = options.minimumTaskCost;
    const std::size_t n = deps.size();
    std::vector<std::uint32_t> followers(n, 0);
    for (std::size_t j = 0; j < n; ++j) {
        for (const auto i : deps.of(j)) ++followers[i];
    }

    // Chains: a node with one dependency that has no other follower joins
    // its task. A chain's edges in end at its first node and its edges out
    // start at its last, so numbering chains by first node is topological.
    disjointSets chains(n);
    for (std::uint32_t j = 0; j < n; ++j) {
        const auto d = deps.of(j);
        if (d.size() == 1 && followers[d.front()] == 1) chains.join(d.front(), j);
    }
    const auto [chainOf, chainCount] = numbered(chains, n);

    // Each chain's level: one more than its dependencies' highest, taken
    // at its first node, the only one with dependencies outside it.
    std::vector<std::uint32_t> level(chainCount, 0);
    std::vector<double> chainCost(chainCount, 0.0);
    for (std::size_t j = 0; j < n; ++j) {
        chainCost[chainOf[j]] += cost[j];
        for (const auto i : deps.of(j)) {
            if (chainOf[i] != chainOf[j])
                level[chainOf[j]] = std::max(level[chainOf[j]], level[chainOf[i]] + 1);
        }
    }

    // Chains of one level reach none of each other, so joining them keeps
    // the task graph acyclic and every level as it was. Each level's open
    // task takes the next chain while it holds less than the minimum.
    disjointSets joined(chainCount);
    std::vector<double> joinedCost = chainCost;
    // Each level's open task, by its first chain.
    std::vector<std::optional<std::uint32_t>> open(level.empty() ? 0 : *std::ranges::max_element(level) + 1);
    for (std::uint32_t c = 0; c < chainCount; ++c) {
        auto& at = open[level[c]];
        if (at && joinedCost[*at] < minimum) {
            joined.join(*at, c);
            joinedCost[*at] += chainCost[c];
        } else {
            at = c;
        }
    }
    const auto [taskOfChain, taskCount] = numbered(joined, chainCount);

    // Each task's cost in parallel: its nodes', and each dependency of one
    // of them on another task's (dependencyCost).
    taskList out;
    // Each node by its task, and each task that waits by the task it waits
    // for, grouped by counting (byNode) rather than in a list a task.
    std::vector<nodeItem<std::uint32_t>> members;
    std::vector<nodeItem<std::uint32_t>> edges;
    members.reserve(n);
    std::vector<double> taskCost(taskCount, 0.0);
    out.waits.assign(taskCount, 0);
    for (std::uint32_t j = 0; j < n; ++j) {
        const auto t = taskOfChain[chainOf[j]];
        members.push_back({.node = t, .item = j});
        taskCost[t] += cost[j];
        out.serialWork += cost[j];
        for (const auto i : deps.of(j)) {
            if (const auto from = taskOfChain[chainOf[i]]; from != t) {
                edges.push_back({.node = from, .item = t});
                taskCost[t] += dependencyCost;
            }
        }
    }
    auto byTask = byNode(taskCount, std::move(members));
    out.nodes = std::move(byTask.items);
    out.start = std::move(byTask.start);
    auto next = byNode(taskCount, std::move(edges));
    out.successors.reserve(next.items.size());
    out.successorStart.reserve(taskCount + 1);
    for (std::uint32_t t = 0; t < taskCount; ++t) {
        const auto s = std::span(next.items).subspan(next.start[t], next.start[t + 1] - next.start[t]);
        std::ranges::sort(s);
        const auto distinct = s.first(static_cast<std::size_t>(std::ranges::unique(s).begin() - s.begin()));
        for (const auto u : distinct) ++out.waits[u];
        out.successors.insert(out.successors.end(), distinct.begin(), distinct.end());
        out.successorStart.push_back(static_cast<std::uint32_t>(out.successors.size()));
    }
    // The costliest path through the task graph. Every edge between tasks
    // rises a level, so tasks taken level by level come after every task
    // they wait for; each task's entry holds when it can start, then when
    // it finishes.
    std::vector<std::uint32_t> taskLevel(taskCount, 0);
    for (std::uint32_t c = 0; c < chainCount; ++c) taskLevel[taskOfChain[c]] = level[c];
    std::vector<std::uint32_t> byLevel(taskCount);
    std::iota(byLevel.begin(), byLevel.end(), std::uint32_t{0});
    std::ranges::stable_sort(byLevel, {}, [&](std::uint32_t t) { return taskLevel[t]; });
    std::vector<double> finish(taskCount, 0.0);
    for (const auto t : byLevel) {
        finish[t] += taskCost[t];
        out.work += taskCost[t];
        out.criticalPath = std::max(out.criticalPath, finish[t]);
        for (const auto u : out.successorsOf(t)) finish[u] = std::max(finish[u], finish[t]);
    }
    // Every level up to the highest holds a task, since a chain of level
    // L > 0 depends on one of level L − 1. So some level holds two tasks,
    // which cannot reach each other and can run at once, exactly when
    // there are more tasks than levels; otherwise each task waits for the
    // one a level below it, and the tasks form a chain. Tasks that can run
    // at once run in parallel when they save enough on the threads.
    out.parallel = taskCount > open.size() && options.threads > 1 &&
                   out.savingAt(options.threads) >= options.minimumParallelSaving;
    return out;
}

std::vector<std::uint32_t> shuffledOrder(const dependencyList& deps, std::uint64_t seed) {
    const std::size_t n = deps.size();
    // Each node's followers, node by node, and the dependencies each node
    // still waits for.
    std::vector<nodeItem<std::uint32_t>> edges;
    edges.reserve(deps.before.size());
    std::vector<std::uint32_t> waiting(n);
    for (std::uint32_t j = 0; j < n; ++j) {
        const auto d = deps.of(j);
        waiting[j] = static_cast<std::uint32_t>(d.size());
        for (const auto i : d) edges.push_back({.node = i, .item = j});
    }
    const auto followers = byNode(n, std::move(edges));
    std::vector<std::uint32_t> ready;
    for (std::uint32_t j = 0; j < n; ++j) {
        if (waiting[j] == 0) ready.push_back(j);
    }
    rng draws(seed);
    std::vector<std::uint32_t> order;
    order.reserve(n);
    while (!ready.empty()) {
        const auto k = static_cast<std::size_t>(draws.next() % ready.size());
        std::swap(ready[k], ready.back());
        const auto j = ready.back();
        ready.pop_back();
        order.push_back(j);
        for (const auto f : followers.of(j)) {
            if (--waiting[f] == 0) ready.push_back(f);
        }
    }
    return order;
}

nodeSchedule scheduleOf(std::span<const std::reference_wrapper<const nodeInfo>> nodes,
                        std::span<const metadata::pipeRef> feedback, std::span<const metadata::pipeRef> used,
                        const scheduleOptions& options, std::span<const std::shared_ptr<aPipe>> privates) {
    nodeSchedule out;
    auto [dependencies, pipes] = analysisOf(nodes, feedback, used, privates);
    out.dependencies = std::move(dependencies);
    out.pipes = std::move(pipes);
    std::vector<double> cost;
    cost.reserve(nodes.size());
    for (const nodeInfo& n : nodes) cost.push_back(n.cost);
    out.tasks = tasksOf(out.dependencies, cost, options);
    out.counters.waiting = std::vector<std::atomic<std::uint32_t>>(out.tasks.size());
    if (options.shuffleSeed) {
        out.serialOrder = shuffledOrder(out.dependencies, *options.shuffleSeed);
    } else {
        out.serialOrder.resize(nodes.size());
        std::iota(out.serialOrder.begin(), out.serialOrder.end(), std::uint32_t{0});
    }
    return out;
}

}  // namespace primitives
