// The server's protocol engine: reads and validates IR, builds plans, and
// drives the audio engine (protocol.md §6.4, §7). Has no JUCE dependency,
// so it is tested without devices. Runs on the control thread.

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <proj/primitives/engine.h>
#include <proj/protocol/osc.h>
#include <proj/protocol/serverSession.h>

namespace server {

/// Reads, validates, and builds @p ir as plan @p planId. On failure,
/// returns null and sets @p error to messages that name the failing node.
std::unique_ptr<primitives::plan> buildFromIr(primitives::engine& audio, std::string_view ir, std::int32_t planId,
                                              std::string& error);

/// Implements the protocol's engine interface on a primitives engine.
class engineHost final : public protocol::engine {
public:
    /// Drives @p audio, which must outlive the host.
    explicit engineHost(primitives::engine& audio) : audio_(audio) {}

    /// Reads, validates, and builds @p ir. Errors name the failing node.
    std::optional<std::string> loadPlan(std::int32_t planId, std::string ir) override;
    /// True for plans loaded and not yet played.
    bool hasPlan(std::int32_t planId) const override { return loaded_.count(planId) != 0; }
    /// Schedules the swap to a loaded plan.
    void play(std::int32_t planId, float fade, std::string_view quant) override;
    /// Fades out the active plan.
    void stop(float fade) override;
    /// Queues a control pipe write.
    void setControl(std::int32_t index, float value) override { audio_.setControl(index, value); }
    /// Reads a control pipe.
    float control(std::int32_t index) const override { return audio_.control(index); }
    /// Engine counters plus device xruns.
    protocol::serverCounters counters() const override;
    /// Requests shutdown.
    void quit() override { quit_ = true; }

    /// True after `/quit`.
    bool quitRequested() const noexcept { return quit_; }
    /// Records the device's xrun count (audio device thread or control thread).
    void setXruns(std::int32_t n) noexcept { xruns_.store(n, std::memory_order_relaxed); }

    /// Frees retired plans and returns the notifications to broadcast:
    /// `/plan.active` and `/plan.retired` (protocol.md §5).
    std::vector<protocol::message> poll();

private:
    primitives::engine& audio_;
    std::map<std::int32_t, std::unique_ptr<primitives::plan>> loaded_;
    std::atomic<std::int32_t> xruns_{0};
    bool quit_ = false;
};

} // namespace server
