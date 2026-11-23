// Internal: a swap between two plans, with the merged execution order for
// its crossfade (primitives.md §9.2, §9.3). Built on the control thread,
// run by the audio thread. Not part of the public API.

#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <espresso/primitives/engine.h>
#include <espresso/primitives/plan.h>
#include <espresso/primitives/realtime.h>

namespace primitives {

/// A swap from `outgoing` to `incoming`. The audio thread takes one only
/// when no fade is running.
struct transition {
    /// What a node does during the fade.
    enum class role : std::uint8_t { steady, rising, falling };
    /// A node in the merged execution order.
    struct entry {
        std::shared_ptr<primitive> node;
        role what = role::steady;
        bool ramp = false;  // applies the fade gain; see fadeRamps in transition.cpp
    };
    /// Why the audio thread handed the transition back.
    enum class fate : std::uint8_t { pending, replaced, stale, finished };

    /// The engine::play() call that made it; a rebuild keeps it.
    std::uint64_t seq = 0;
    /// Becomes the active plan at the swap; afterwards holds the plan it replaced.
    std::shared_ptr<plan> incoming;
    /// The plan it was built against, kept until the transition is freed.
    std::shared_ptr<plan> outgoing;
    std::vector<entry> order;
    /// How `order` may run in parallel (`threads.md` §4, §5), when the
    /// swap is between two plans, or between none. Empty for a swap from
    /// or to no plan, whose merged order is that plan's own order: its
    /// fade runs that plan's schedule (`alone`).
    std::optional<nodeSchedule> merged;
    /// The one plan of a swap from or to no plan; null otherwise.
    std::shared_ptr<plan> alone;
    /// The feedback pipes during the fade, sorted (`threads.md` §4.1.3).
    std::vector<metadata::pipeRef> feedback;
    std::vector<std::pair<std::shared_ptr<primitive>, std::shared_ptr<primitive>>>
        adopt;  // incoming, outgoing
    double fade = 0.0;
    quantization quant = quantization::none;
    double target = NAN;  // beat to swap at; NaN swaps at the next block
    double grid = 0.0;    // beats between grid points; 0 without quantization
    std::int64_t fadeSamples = 0;
    std::int64_t fadeDone = 0;
    bool waited = false;  // waited for a running fade while pending
    fate state = fate::pending;

    /// How `order` may run in parallel: the merged schedule, or the one
    /// plan's own.
    nodeSchedule& schedule() noexcept ESPRESSO_NONBLOCKING { return merged ? *merged : alone->schedule; }
};

/// Builds the swap from @p outgoing to @p incoming, either of which may be
/// null, with a crossfade of @p fade seconds (finite, at least 0), its
/// schedule made with @p options; a swap from or to no plan makes none
/// and runs that plan's.
std::unique_ptr<transition> makeTransition(std::shared_ptr<plan> incoming, std::shared_ptr<plan> outgoing,
                                           double fade, quantization quant, double sampleRate,
                                           const scheduleOptions& options = {});

}  // namespace primitives
