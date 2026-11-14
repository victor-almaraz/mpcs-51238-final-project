// Command-line arguments for the executables, as string views.

#pragma once

#include <charconv>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace apps {

// The arguments of main(). The C entry point passes them as a raw array:
// the narrow unsafe use at the language boundary (see the modern-cpp
// skill), read here once.
inline std::vector<std::string_view> arguments(int argc, char** argv) { return {argv, argv + argc}; }

// @p text as a number when it is one and nothing else.
template <typename T>
std::optional<T> parseNumber(std::string_view text) {
    T value{};
    if constexpr (std::is_integral_v<T>) {
        const auto [end, error] = std::from_chars(text.begin(), text.end(), value);
        if (error != std::errc{} || end != text.end()) return std::nullopt;
    } else {
        // from_chars for floating point is missing from some standard
        // libraries (lessons.md).
        std::istringstream in{std::string(text)};
        char extra = 0;
        if (!(in >> value) || (in >> extra)) return std::nullopt;
    }
    return value;
}

} // namespace apps
