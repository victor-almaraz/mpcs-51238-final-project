#include "ioStats.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace server {

ioStats::entry& ioStats::at(std::string_view node) {
    const auto it = nodes_.find(node);
    return it != nodes_.end() ? it->second : nodes_[std::string(node)];
}

void ioStats::add(std::span<const std::string> nodes, ioCount what, std::uint64_t n) {
    const std::scoped_lock lock(mutex_);
    for (const auto& node : nodes) at(node).counts.add(what, n);
}

void ioStats::add(std::string_view node, ioCount what, std::uint64_t n) {
    const std::scoped_lock lock(mutex_);
    at(node).counts.add(what, n);
}

void ioStats::error(std::span<const std::string> nodes, std::string_view message) {
    const std::scoped_lock lock(mutex_);
    for (const auto& node : nodes) {
        entry& e = at(node);
        e.counts.add(ioCount::errors, 1);
        e.message = message;
        if (e.unreported < std::numeric_limits<std::int32_t>::max()) ++e.unreported;
        errors_.fetch_add(1, std::memory_order_relaxed);
    }
}

void ioStats::error(std::string_view node, std::string_view message) {
    const std::array<std::string, 1> one{std::string(node)};
    error(one, message);
}

nodeCounts ioStats::of(std::string_view node) const {
    const std::scoped_lock lock(mutex_);
    const auto it = nodes_.find(node);
    return it == nodes_.end() ? nodeCounts{} : it->second.counts;
}

std::vector<ioError> ioStats::due(std::chrono::steady_clock::time_point now) {
    std::vector<ioError> out;
    const std::scoped_lock lock(mutex_);
    for (auto& [node, e] : nodes_) {
        if (e.unreported == 0 || (e.reported && now - *e.reported < reportInterval)) continue;
        out.push_back({.node = node, .message = e.message, .count = e.unreported});
        e.unreported = 0;
        e.reported = now;
    }
    return out;
}

}  // namespace server
