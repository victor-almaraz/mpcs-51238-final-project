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

std::unique_ptr<primitives::plan> buildFromIr(primitives::engine& audio, std::string_view ir, std::int32_t planId,
                                              std::string& error) {
    const auto r = ir::read(ir);
    if (!r.ok()) {
        error = r.text();
        return nullptr;
    }
    try {
        return audio.build(*r.graph, planId);
    } catch (const primitives::buildError& e) {
        error = (e.node().empty() ? std::string() : "node " + e.node() + ": ") + e.what();
        return nullptr;
    }
}

std::optional<std::string> engineHost::loadPlan(std::int32_t planId, std::string ir) {
    std::string error;
    auto plan = buildFromIr(audio_, ir, planId, error);
    if (!plan) return error;
    loaded_[planId] = std::move(plan);
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
    for (auto& f : audio_.io().takeFaults()) {
        out.push_back(protocol::failure(0, protocol::errorCode::signalFault, "node " + std::move(f)));
    }
    return out;
}

} // namespace server
