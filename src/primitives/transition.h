// Internal: a swap between two plans, with the merged execution order for
// its crossfade (primitives.md §9.2, §9.3). Built on the control thread,
// run by the audio thread. Not part of the public API.

#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <espresso/primitives/engine.h>
#include <espresso/primitives/plan.h>

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
    /// How `order` may run in parallel (`threads.md` §4, §5).
    nodeSchedule schedule;
    /// The feedback pipes during the fade, sorted (`threads.md` §4.1.3).
    std::vector<metadata::pipeRef> feedback;
    std::vector<std::pair<std::shared_ptr<primitive>, std::shared_ptr<primitive>>>
        adopt;  // incoming, outgoing
    /// Every pipe either plan touches, sorted and unique.
    std::vector<metadata::pipeRef> used;
    double fade = 0.0;
    quantization quant = quantization::none;
    double target = NAN;  // beat to swap at; NaN swaps at the next block
    double grid = 0.0;    // beats between grid points; 0 without quantization
    std::int64_t fadeSamples = 0;
    std::int64_t fadeDone = 0;
    bool waited = false;  // waited for a running fade while pending
    fate state = fate::pending;
};

/// Builds the swap from @p outgoing to @p incoming, either of which may be
/// null, with a crossfade of @p fade seconds (finite, at least 0).
std::unique_ptr<transition> makeTransition(std::shared_ptr<plan> incoming, std::shared_ptr<plan> outgoing,
                                           double fade, quantization quant, double sampleRate);

}  // namespace primitives
