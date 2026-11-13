#include "engineHost.h"

#include <proj/ir/ir.h>
#include <proj/protocol/messages.h>

#include <algorithm>
#include <limits>

namespace server {

namespace {

std::int32_t clamp32(std::uint64_t v) {
    return static_cast<std::int32_t>(std::min<std::uint64_t>(v, std::numeric_limits<std::int32_t>::max()));
}

} // namespace

std::optional<std::string> engineHost::loadPlan(std::int32_t planId, std::string ir) {
    const auto r = ir::read(ir);
    if (!r.ok()) return r.text();
    try {
        loaded_[planId] = audio_.build(*r.graph, planId);
    } catch (const primitives::buildError& e) {
        return (e.node().empty() ? std::string() : "node " + e.node() + ": ") + e.what();
    }
    return std::nullopt;
}

void engineHost::play(std::int32_t planId, float fade, std::string_view quant) {
    const auto it = loaded_.find(planId);
    if (it == loaded_.end()) return;  // the session checks hasPlan first
    audio_.play(std::move(it->second), fade, primitives::parseQuantization(quant).value_or(primitives::quantization::none));
    loaded_.erase(it);
}

void engineHost::stop(float fade) { audio_.stop(fade); }

protocol::serverCounters engineHost::counters() const {
    const auto c = audio_.counters();
    protocol::serverCounters out;
    out.load = c.load;
    out.missedDeadlines = clamp32(c.missedDeadlines);
    out.xruns = xruns_.load(std::memory_order_relaxed);
    out.drops = clamp32(c.drops);
    out.overflows = clamp32(c.overflows);
    out.activePlan = c.activePlan;
    return out;
}

std::vector<protocol::message> engineHost::poll() {
    std::vector<protocol::message> out;
    for (const auto& n : audio_.collect()) {
        const auto address = n.what == primitives::notice::kind::activated ? protocol::address::planActive
                                                                           : protocol::address::planRetired;
        out.push_back({std::string(address), {n.plan}});
    }
    return out;
}

} // namespace server
