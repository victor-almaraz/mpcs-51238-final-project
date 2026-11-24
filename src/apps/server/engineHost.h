// The server's protocol engine: reads and validates IR, builds plans, and
// drives the audio engine (protocol.md §6.4, §7). Has no JUCE dependency,
// so it is tested without devices. Runs on the control thread.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/primitives/engine.h>
#include <espresso/protocol/osc.h>
#include <espresso/protocol/serverSession.h>

#include "ioStats.h"

namespace server {

/// A plan built from IR, with the page port its IR names (io.md §12.2):
/// server configuration, which the engine does not read, kept beside the
/// plan rather than on it.
struct builtPlan {
    /// The plan; null when it could not be built.
    std::shared_ptr<primitives::plan> plan;
    /// The port the server listens for pages on while the plan is active.
    std::optional<std::uint16_t> pagePort;
};

/// Reads, validates, and builds @p ir as plan @p planId, with its page
/// port. On failure, returns no plan and sets @p error to messages that
/// name the failing node, and to the node or buffer, when one failed.
/// Never throws.
builtPlan buildFromIr(primitives::engine& audio, std::string_view ir, std::int32_t planId,
                      protocol::planError& error);

/// Implements the protocol's engine interface on a primitives engine.
///
/// A plan ID names one plan from `/ir.end` until `/plan.retired`. A plan
/// plays once: `/play` hands it to the engine, so a second `/play` of the
/// same ID fails as an unknown plan. Loading an ID that is loaded or
/// playing fails; after `/plan.retired` the ID can be loaded again. At
/// most maxLoaded plans wait loaded and unplayed; loading another drops
/// the one loaded first.
class engineHost final : public protocol::engine {
public:
    /// Most plans that are loaded and not yet played.
    static constexpr std::size_t maxLoaded = 16;

    /// Drives @p audio, which must outlive the host. Buffers save to files
    /// under @p root, the project root (samples.md §18).
    explicit engineHost(primitives::engine& audio, std::filesystem::path root = ".")
        : audio_(audio), root_(std::move(root)) {}

    /// Reads, validates, and builds @p ir. Errors name the failing node.
    std::optional<protocol::planError> loadPlan(std::int32_t planId, std::string ir) override;
    /// True for plans loaded and not yet played.
    bool hasPlan(std::int32_t planId) const override { return findLoaded(planId) != loaded_.end(); }
    /// Schedules the swap to a loaded plan and consumes it.
    void play(std::int32_t planId, float fade, std::string_view quant) override;
    /// Fades out the active plan.
    void stop(float fade) override;
    /// Queues a control pipe write.
    void setControl(std::int32_t index, float value) override { audio_.setControl(index, value); }
    /// Reads a control pipe.
    float control(std::int32_t index) const override { return audio_.control(index); }
    /// Engine counters plus device xruns, and the beat clock's tempo,
    /// position, and meter.
    protocol::serverCounters counters() const override;
    /// Requests shutdown.
    void quit() override { quit_ = true; }

    /// True after `/quit`.
    bool quitRequested() const noexcept { return quit_; }
    /// The active plan's page port, if it has one (io.md §12.2).
    std::optional<std::uint16_t> pagePort() const noexcept { return pagePort_; }

    /// Serial and web I/O counts and errors, which the bridges fill
    /// (io.md §10).
    ioStats& io() noexcept { return stats_; }

    /// Records the device's xrun count (audio device thread or control thread).
    void setXruns(std::int32_t n) noexcept { xruns_.store(n, std::memory_order_relaxed); }

    /// Frees retired plans, writes buffer saves, and returns the
    /// notifications to broadcast: `/plan.active`, `/plan.retired`,
    /// `/buffer.saved`, an unsolicited `/fail` for each signal fault and
    /// each save that failed (protocol.md §5), and `/io.error` for each
    /// node with I/O errors to report (io.md §10.2).
    std::vector<protocol::message> poll();

private:
    using loadedPlans = std::vector<std::pair<std::int32_t, builtPlan>>;

    loadedPlans::const_iterator findLoaded(std::int32_t planId) const;

    primitives::engine& audio_;
    std::filesystem::path root_;
    loadedPlans loaded_;              // in load order
    // Plans played and not yet retired, with their page ports.
    std::map<std::int32_t, std::optional<std::uint16_t>> playing_;
    std::optional<std::uint16_t> pagePort_;  // the active plan's
    std::atomic<std::int32_t> xruns_{0};
    ioStats stats_;
    bool quit_ = false;
};

} // namespace server
