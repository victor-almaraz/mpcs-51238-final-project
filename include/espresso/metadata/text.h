/// @file
/// Text helpers for messages, shared by the IR validator, the hazard
/// analysis, and the compiler.

#pragma once

#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace metadata {

/// @p v as a message shows it: up to six significant digits, as
/// `std::ostream` writes a double.
inline std::string formatNumber(double v) {
    std::ostringstream out;
    out << v;
    return out.str();
}

/// @p names separated by commas, each after @p prefix: `:low, :band`.
inline std::string joinNames(std::span<const std::string_view> names, std::string_view prefix = {}) {
    std::string out;
    for (const auto name : names) {
        if (!out.empty()) out += ", ";
        out += prefix;
        out += name;
    }
    return out;
}

/// The `text()` of each item of @p items, one per line.
template <typename Range>
std::string joinLines(const Range& items) {
    std::string out;
    for (const auto& item : items) {
        if (!out.empty()) out += '\n';
        out += item.text();
    }
    return out;
}

}  // namespace metadata
