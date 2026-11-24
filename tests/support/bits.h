// Compares renders bit for bit (threads.md §1.3, §9.1): -0 and +0 differ,
// as do two NaNs of different payloads, which operator== on floats would
// equate or tell apart wrongly.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace support {

/// Where @p actual first differs from @p expected in bits, for a failure
/// message ("sample 4410: 0x80000000 (-0) where the reference has
/// 0x00000000 (0)"), or empty when both hold the same bits. Use it as
/// `CHECK(support::bitDifference(a, b) == "")`, so a failure shows it.
inline std::string bitDifference(std::span<const float> actual, std::span<const float> expected) {
    std::ostringstream out;
    if (actual.size() != expected.size()) {
        out << actual.size() << " samples where the reference has " << expected.size();
        return out.str();
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto a = std::bit_cast<std::uint32_t>(actual[i]);
        const auto e = std::bit_cast<std::uint32_t>(expected[i]);
        if (a == e) continue;
        out << "sample " << i << ": 0x" << std::hex << std::setw(8) << std::setfill('0') << a << std::dec
            << " (" << actual[i] << ") where the reference has 0x" << std::hex << std::setw(8) << e
            << std::dec << " (" << expected[i] << ")";
        return out.str();
    }
    return {};
}

/// As bitDifference() for one channel, for renders of several channels:
/// the first channel that differs, and where.
inline std::string bitDifference(std::span<const std::vector<float>> actual,
                                 std::span<const std::vector<float>> expected) {
    if (actual.size() != expected.size()) {
        return std::to_string(actual.size()) + " channels where the reference has " +
               std::to_string(expected.size());
    }
    for (std::size_t c = 0; c < actual.size(); ++c) {
        if (auto d = bitDifference(actual[c], expected[c]); !d.empty())
            return "channel " + std::to_string(c) + ", " + d;
    }
    return {};
}

}  // namespace support
