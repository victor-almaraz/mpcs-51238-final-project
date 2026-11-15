// Swap transitions: role assignment, fade placement, and the merged
// execution order of a crossfade (primitives.md §9.2, §9.3).

#include "transition.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <queue>
#include <unordered_map>

#include <proj/metadata/primitiveSpec.h>

namespace primitives {

using metadata::pipeRef;

namespace {

using role = transition::role;

// A node taking part in a transition.
struct item {
    std::shared_ptr<primitive> node;
    role what;
    std::reference_wrapper<const nodeInfo> info;
    std::reference_wrapper<const plan> owner;
    std::size_t rank;  // incoming nodes at even ranks, retiring at odd: ties keep each plan's order
};

// Matches incoming nodes to outgoing ones by ID and shape (§9.2): matched
// nodes are steady and adopt state; the rest of the incoming plan rises
// and the rest of the outgoing plan falls.
std::vector<item>
assignRoles(const std::shared_ptr<plan>& incoming, const std::shared_ptr<plan>& outgoing,
            std::vector<std::pair<std::shared_ptr<primitive>, std::shared_ptr<primitive>>>& adopt) {
    std::vector<item> items;
    std::vector<bool> adopted(outgoing ? outgoing->nodes.size() : 0, false);
    std::unordered_map<std::string_view, std::size_t> outgoingIds;
    if (outgoing) {
        for (std::size_t j = 0; j < outgoing->nodes.size(); ++j)
            outgoingIds.emplace(outgoing->nodes[j]->id(), j);
    }
    if (incoming) {
        for (std::size_t i = 0; i < incoming->nodes.size(); ++i) {
            const auto& node = incoming->nodes[i];
            role what = role::rising;
            if (const auto it = outgoingIds.find(node->id()); it != outgoingIds.end()) {
                const std::size_t j = it->second;
                if (outgoing->info[j].shape == incoming->info[i].shape) {  // §9.2.2
                    adopt.emplace_back(node, outgoing->nodes[j]);
                    adopted[j] = true;
                    what = role::steady;
                }
            }
            items.push_back({node, what, std::cref(incoming->info[i]), std::cref(*incoming), 2 * i});
        }
    }
    if (outgoing) {
        for (std::size_t j = 0; j < outgoing->nodes.size(); ++j) {
            if (!adopted[j]) {
                items.push_back({outgoing->nodes[j], role::falling, std::cref(outgoing->info[j]),
                                 std::cref(*outgoing), 2 * j + 1});
            }
        }
    }
    return items;
}

// Fade gain is applied once per signal path (§9.3.2), where the path
// leaves the fading subgraph: a fading node ramps when a node in another
// role reads its audio, or when it writes the device (a sink). Nodes
// inside a fading subgraph pass audio through unscaled, so chains,
// nonlinear stages (a carrier modulated by a new modulator), and signal
// circulating in a feedback loop are all scaled exactly once. Where old
// and new plans share a pipe, each reader applies its own ramp, which
// splits the mixed signal between them.
std::vector<bool> fadeRamps(const std::vector<item>& items) {
    std::map<pipeRef, std::vector<std::size_t>> audioReaders;
    for (std::size_t k = 0; k < items.size(); ++k) {
        for (auto r : items[k].info.get().reads) {
            if (r.kind == metadata::pipeKind::audio) audioReaders[r].push_back(k);
        }
    }
    std::vector<bool> ramps(items.size(), false);
    for (std::size_t k = 0; k < items.size(); ++k) {
        if (items[k].what == role::steady) continue;
        const auto spec = metadata::findPrimitive(items[k].node->type());
        bool exit = spec && spec->get().kind == metadata::role::sink;
        for (auto w : items[k].info.get().writes) {
            if (w.kind != metadata::pipeKind::audio) continue;
            const auto it = audioReaders.find(w);
            if (it == audioReaders.end()) continue;
            for (std::size_t reader : it->second) {
                if (reader != k && items[reader].what != items[k].what) exit = true;
            }
        }
        ramps[k] = exit;
    }
    return ramps;
}

// Orders @p items so every writer of a pipe runs before its readers
// (§9.3.1); ties keep each plan's order. Each plan alone is acyclic, so a
// cycle in the merge passes through a retiring node. The order cuts each
// cycle at its earliest retiring node: that node runs before the rest of
// its cycle, so it reads only the audio that nodes already run have
// written in the block and the last values of control pipes.
std::vector<transition::entry> mergeOrder(const std::vector<item>& items, const std::vector<bool>& ramps) {
    // Edges from each writer of a pipe to each of its readers.
    std::map<pipeRef, std::vector<std::size_t>> writers;
    for (std::size_t k = 0; k < items.size(); ++k) {
        for (auto w : items[k].info.get().writes) {
            if (!items[k].owner.get().isFeedback(w)) writers[w].push_back(k);
        }
    }
    std::vector<std::vector<std::size_t>> successors(items.size());
    std::vector<std::size_t> indegree(items.size(), 0);
    for (std::size_t k = 0; k < items.size(); ++k) {
        for (auto r : items[k].info.get().reads) {
            if (items[k].owner.get().isFeedback(r)) continue;
            const auto it = writers.find(r);
            if (it == writers.end()) continue;
            for (std::size_t w : it->second) {
                if (w == k) continue;
                successors[w].push_back(k);
                ++indegree[k];
            }
        }
    }

    auto later = [&](std::size_t a, std::size_t b) { return items[a].rank > items[b].rank; };
    std::priority_queue<std::size_t, std::vector<std::size_t>, decltype(later)> ready(later);
    for (std::size_t k = 0; k < items.size(); ++k) {
        if (indegree[k] == 0) ready.push(k);
    }
    std::vector<bool> done(items.size(), false);
    // True when node @p k reaches itself through nodes not yet ordered.
    auto onCycle = [&](std::size_t k) {
        std::vector<bool> seen(items.size(), false);
        std::vector<std::size_t> stack(successors[k]);
        while (!stack.empty()) {
            const std::size_t x = stack.back();
            stack.pop_back();
            if (x == k) return true;
            if (done[x] || seen[x]) continue;
            seen[x] = true;
            stack.insert(stack.end(), successors[x].begin(), successors[x].end());
        }
        return false;
    };
    // The next node to order when every node left waits on another: the
    // earliest retiring node on a cycle. A node between two cycles also
    // waits, but cutting it would break no cycle. Without a retiring node
    // on a cycle (plans that were not validated), the earliest node left.
    auto cut = [&] {
        std::size_t best = items.size();
        std::size_t first = items.size();
        for (std::size_t k = 0; k < items.size(); ++k) {
            if (done[k]) continue;
            if (first == items.size() || items[k].rank < items[first].rank) first = k;
            const bool earlier = best == items.size() || items[k].rank < items[best].rank;
            if (earlier && items[k].what == role::falling && onCycle(k)) best = k;
        }
        return best < items.size() ? best : first;
    };

    auto next = [&] {
        if (ready.empty()) return cut();
        const std::size_t top = ready.top();
        ready.pop();
        return top;
    };

    std::vector<transition::entry> order;
    order.reserve(items.size());
    while (order.size() < items.size()) {
        const std::size_t k = next();
        order.push_back({items[k].node, items[k].what, ramps[k]});
        done[k] = true;
        for (std::size_t s : successors[k]) {
            if (--indegree[s] == 0 && !done[s]) ready.push(s);
        }
    }
    return order;
}

}  // namespace

std::unique_ptr<transition> makeTransition(std::shared_ptr<plan> incoming, std::shared_ptr<plan> outgoing,
                                           double fade, quantization quant, double sampleRate) {
    auto t = std::make_unique<transition>();
    t->fade = fade;
    t->quant = quant;
    // Within the range of std::int64_t and exact in a double.
    constexpr double longest = 0x1p53;
    t->fadeSamples = std::llround(std::clamp(fade * sampleRate, 0.0, longest));
    const auto items = assignRoles(incoming, outgoing, t->adopt);
    t->order = mergeOrder(items, fadeRamps(items));
    // Each plan's list is sorted and unique, so their union is too.
    static const std::vector<pipeRef> none;
    const auto& a = incoming ? incoming->used : none;
    const auto& b = outgoing ? outgoing->used : none;
    std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(t->used));
    t->incoming = std::move(incoming);
    t->outgoing = std::move(outgoing);
    return t;
}

}  // namespace primitives
