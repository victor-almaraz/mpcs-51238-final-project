#include "engineHost.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/ir/ir.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/plan.h>
#include <espresso/protocol/messages.h>
#include <espresso/protocol/osc.h>
#include <espresso/protocol/serverSession.h>

#include "samples.h"

namespace server {

namespace {

std::int32_t clamp32(std::uint64_t v) {
    return static_cast<std::int32_t>(std::min<std::uint64_t>(v, std::numeric_limits<std::int32_t>::max()));
}

} // namespace

std::shared_ptr<primitives::plan> buildFromIr(primitives::engine& audio, std::string_view ir,
                                              std::int32_t planId, protocol::planError& error) {
    const auto r = ir::read(ir);
    if (!r.graph) {  // r.ok(), written so the access below is checked
        // The first error names a node when it is about one.
        const std::string node = r.errors.empty() ? std::string() : r.errors.front().node;
        error = {.message = r.text(),
                 .subject = node.empty() ? protocol::failSubject::none : protocol::failSubject::node,
                 .id = node};
        return nullptr;
    }
    try {
        return audio.build(*r.graph, planId);
    } catch (const primitives::buildError& e) {
        using protocol::failSubject;
        auto subject = failSubject::none;
        if (!e.node().empty())
            subject = e.about() == primitives::buildError::subject::buffer ? failSubject::buffer
                                                                           : failSubject::node;
        const std::string_view name = protocol::subjectName(subject);
        error = {.message =
                     (name.empty() ? std::string() : std::string(name) + " " + e.node() + ": ") + e.what(),
                 .subject = subject,
                 .id = e.node()};
    } catch (const std::exception& e) {  // such as a buffer too large to allocate
        error = protocol::planError::plain(std::string("cannot build the plan: ") + e.what());
    }
    return nullptr;
}

engineHost::loadedPlans::const_iterator engineHost::findLoaded(std::int32_t planId) const {
    return std::ranges::find_if(loaded_, [&](const auto& p) { return p.first == planId; });
}

std::optional<protocol::planError> engineHost::loadPlan(std::int32_t planId, std::string ir) {
    const std::string name = "plan " + std::to_string(planId);
    if (hasPlan(planId))
        return protocol::planError::plain(
            name + " is loaded and not yet played; load the new plan under another ID");
    if (playing_.contains(planId))
        return protocol::planError::plain(
            name + " is playing or waiting to play; its ID is free after /plan.retired");
    protocol::planError error;
    auto plan = buildFromIr(audio_, ir, planId, error);
    if (!plan) return error;
    if (loaded_.size() >= maxLoaded) loaded_.erase(loaded_.begin());
    loaded_.emplace_back(planId, std::move(plan));
    return std::nullopt;
}

void engineHost::play(std::int32_t planId, float fade, std::string_view quant) {
    const auto it = findLoaded(planId);
    if (it == loaded_.end()) return;  // the session checks hasPlan first
    auto plan = it->second;
    loaded_.erase(it);  // a plan plays once
    playing_.insert(planId);
    audio_.play(std::move(plan), fade,
                primitives::parseQuantization(quant).value_or(primitives::quantization::none));
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
    out.tempo = static_cast<float>(audio_.tempo());
    out.beat = audio_.beat();
    out.meter = audio_.meter();
    return out;
}

std::vector<protocol::message> engineHost::poll() {
    std::vector<protocol::message> out;
    for (const auto& n : audio_.collect()) {
        const bool active = n.what == primitives::notice::kind::activated;
        if (!active) playing_.erase(n.plan);
        out.push_back(
            {.address = std::string(active ? protocol::address::planActive : protocol::address::planRetired),
             .args = {n.plan}});
    }
    for (auto& f : audio_.io().takeFaults()) {
        out.push_back(protocol::failure(0, protocol::errorCode::signalFault, "node " + std::move(f)));
    }
    for (auto& s : writeSaves(audio_.io(), root_)) {
        if (s.error.empty()) {
            out.push_back({.address = std::string(protocol::address::bufferSaved),
                           .args = {std::move(s.bufferId), std::move(s.file)}});
        } else {
            out.push_back(protocol::failure(0, protocol::errorCode::fileError,
                                            protocol::planError{.message = s.message(),
                                                                .subject = protocol::failSubject::node,
                                                                .id = std::move(s.node)}));
        }
    }
    return out;
}

} // namespace server
