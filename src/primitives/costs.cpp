// The estimated cost of each primitive type (threads.md §5.2).

#include "costs.h"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace primitives {

namespace {

// The largest cost per node of each type in 10 s Release renders of every
// example and the load piece, on the reference machine (Intel Core
// i5-7360U, 48 kHz, 256-sample blocks; `espresso-server --render
// --profile`), rounded up. Types that cost less than the default are left
// out. Sorted by type.
// clang-format off: one type a line
constexpr std::array costs{
    typeCost{.type = "add", .nanoseconds = 3},
    typeCost{.type = "adsr", .nanoseconds = 6},
    typeCost{.type = "allpass", .nanoseconds = 26},
    typeCost{.type = "audioOut", .nanoseconds = 17},
    typeCost{.type = "biquad", .nanoseconds = 11},
    typeCost{.type = "bufferRead", .nanoseconds = 77},
    typeCost{.type = "bufferWrite", .nanoseconds = 17},
    typeCost{.type = "clamp", .nanoseconds = 3},
    typeCost{.type = "comb", .nanoseconds = 45},
    typeCost{.type = "crossfade", .nanoseconds = 5},
    typeCost{.type = "delayLine", .nanoseconds = 25},
    typeCost{.type = "exp", .nanoseconds = 7},
    typeCost{.type = "fft", .nanoseconds = 18},
    typeCost{.type = "grains", .nanoseconds = 260},
    typeCost{.type = "ifft", .nanoseconds = 19},
    typeCost{.type = "ladder", .nanoseconds = 57},
    typeCost{.type = "lfo", .nanoseconds = 24},
    typeCost{.type = "mix", .nanoseconds = 3},
    typeCost{.type = "multiply", .nanoseconds = 5},
    typeCost{.type = "noise", .nanoseconds = 7},
    typeCost{.type = "pan", .nanoseconds = 25},
    typeCost{.type = "pulseOsc", .nanoseconds = 14},
    typeCost{.type = "pvDelay", .nanoseconds = 110},
    typeCost{.type = "pvFreeze", .nanoseconds = 35},
    typeCost{.type = "samplePlayer", .nanoseconds = 5},
    typeCost{.type = "sawOsc", .nanoseconds = 10},
    typeCost{.type = "sineOsc", .nanoseconds = 21},
    typeCost{.type = "smoother", .nanoseconds = 5},
    typeCost{.type = "sum", .nanoseconds = 5},
    typeCost{.type = "svf", .nanoseconds = 11},
    typeCost{.type = "tanh", .nanoseconds = 13},
    typeCost{.type = "triangleOsc", .nanoseconds = 14},
    typeCost{.type = "triggerNotes", .nanoseconds = 4},
    typeCost{.type = "wavetableOsc", .nanoseconds = 48},
};
// clang-format on

}  // namespace

std::span<const typeCost> typeCosts() noexcept { return costs; }

double costOf(std::string_view type) noexcept {
    const auto it = std::ranges::lower_bound(costs, type, {}, &typeCost::type);
    return it != costs.end() && it->type == type ? it->nanoseconds : defaultCost;
}

}  // namespace primitives
