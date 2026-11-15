// The stub server's engine: accepts IR, schedules swaps on a 120 BPM 4/4
// clock, and reports plan activation and retirement, without audio. Has
// no dependency beyond protocol, so it is tested without the stub's main.

#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <proj/protocol/messages.h>
#include <proj/protocol/serverSession.h>

namespace stub {

/// The clock the stub schedules on.
using clock_type = std::chrono::steady_clock;

/// A protocol engine without audio. It behaves as the real server does
/// where a client can tell: `/play` consumes the plan, so a second
/// `/play` of the same plan fails with `unknownPlan` (`protocol.md` §4); a
/// plan ID that is loaded or in use cannot be loaded again until it is
/// retired; and a pending swap that a newer `/play` or `/stop` replaces is
/// reported retired.
class stubEngine : public protocol::engine {
public:
    /// Counts beats and bars from @p start.
    explicit stubEngine(clock_type::time_point start) : start_(start) {}

    /// Accepts any IR that looks like a JSON object.
    std::optional<std::string> loadPlan(std::int32_t planId, std::string ir) override {
        const auto first = ir.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "IR is empty";
        if (ir[first] != '{') return "IR is not a JSON object";
        const std::string name = "plan " + std::to_string(planId);
        if (hasPlan(planId))
            return name + " is loaded and not yet played; load the new plan under another ID";
        if (inUse(planId)) return name + " is playing or waiting to play; its ID is free after /plan.retired";
        loaded_[planId] = ir.size();
        return std::nullopt;
    }

    /// True for plans loaded and not yet played.
    bool hasPlan(std::int32_t planId) const override { return loaded_.count(planId) != 0; }

    /// Consumes the plan and schedules its swap at the next beat or bar.
    void play(std::int32_t planId, float fade, std::string_view quant) override {
        loaded_.erase(planId);
        if (swap_) retire(swap_->plan, 0.0f);  // replaced before it swapped in
        const double period = quant == "beat" ? secondsPerBeat : quant == "bar" ? 4 * secondsPerBeat : 0.0;
        auto at = clock_type::now();
        if (period > 0.0) {
            const double elapsed = std::chrono::duration<double>(at - start_).count();
            const double next = std::ceil(elapsed / period) * period;
            at = start_ +
                 std::chrono::duration_cast<clock_type::duration>(std::chrono::duration<double>(next));
        }
        swap_ = pendingSwap{.plan = planId, .at = at, .fade = fade};
    }

    /// Cancels a pending swap and retires the active plan after @p fade.
    void stop(float fade) override {
        if (swap_) retire(swap_->plan, 0.0f);
        swap_.reset();
        if (active_ != 0) retire(active_, fade);
        active_ = 0;
    }

    /// Stores a control value.
    void setControl(std::int32_t index, float value) override { controls_[index] = value; }
    /// The stored control value, or 0.
    float control(std::int32_t index) const override {
        const auto it = controls_.find(index);
        return it == controls_.end() ? 0.0f : it->second;
    }

    /// Zeroed counters with the active plan.
    protocol::serverCounters counters() const override {
        protocol::serverCounters c;
        c.activePlan = active_;
        return c;
    }

    /// Requests shutdown.
    void quit() override { quit_ = true; }
    /// True after `/quit`.
    bool quitRequested() const noexcept { return quit_; }

    /// `/plan.active` and `/plan.retired` messages due at @p now.
    std::vector<protocol::message> tick(clock_type::time_point now) {
        std::vector<protocol::message> out;
        if (swap_ && swap_->at <= now) {
            const pendingSwap s = *swap_;
            swap_.reset();
            if (active_ != 0 && active_ != s.plan) retire(active_, s.fade);
            active_ = s.plan;
            out.push_back({.address = std::string(protocol::address::planActive), .args = {s.plan}});
        }
        for (auto it = retiring_.begin(); it != retiring_.end();) {
            if (it->second <= now) {
                out.push_back({.address = std::string(protocol::address::planRetired), .args = {it->first}});
                it = retiring_.erase(it);
            } else {
                ++it;
            }
        }
        return out;
    }

private:
    static constexpr double secondsPerBeat = 60.0 / 120.0;

    struct pendingSwap {
        std::int32_t plan;
        clock_type::time_point at;
        float fade;
    };

    // True while @p plan waits to swap in, plays, or fades out.
    bool inUse(std::int32_t plan) const {
        return (swap_ && swap_->plan == plan) || active_ == plan || retiring_.count(plan) != 0;
    }

    // @p fade is a protocol::validFade(), checked by the session.
    void retire(std::int32_t plan, float fade) {
        retiring_[plan] = clock_type::now() + std::chrono::duration_cast<clock_type::duration>(
                                                  std::chrono::duration<double>(fade));
    }

    clock_type::time_point start_;
    std::map<std::int32_t, std::size_t> loaded_;  // plan ID -> IR bytes
    std::map<std::int32_t, clock_type::time_point> retiring_;
    std::map<std::int32_t, float> controls_;
    std::optional<pendingSwap> swap_;
    std::int32_t active_ = 0;
    bool quit_ = false;
};

}  // namespace stub
