// Internal: the estimated cost of each primitive type, for grouping nodes
// into tasks (threads.md §5.2). Not part of the public API.

#pragma once

#include <span>
#include <string_view>

namespace primitives {

/// A primitive type's estimated cost.
struct typeCost {
    std::string_view type;
    /// Nanoseconds per sample of one node.
    double nanoseconds;
};

/// The measured costs, sorted by type.
std::span<const typeCost> typeCosts() noexcept;

/// The cost of a node of @p type, in nanoseconds per sample: its measured
/// cost, or `defaultCost` for a type no measurement covers.
double costOf(std::string_view type) noexcept;

/// The cost of a type without a measurement.
inline constexpr double defaultCost = 2.0;

/// The estimated cost, in nanoseconds a block, of each dependency of a
/// node on a node of another task when tasks run in parallel: the thread
/// that runs the dependent node reads what the other wrote, a pipe of 1 KB
/// at 256 samples, from another processor's cache (threads.md §6.1.1).
/// Fitted to Release renders of every example at 256-sample blocks on the
/// development Mac; the same at every block size.
inline constexpr double dependencyCost = 600.0;

}  // namespace primitives
