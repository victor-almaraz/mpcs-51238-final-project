// Command-line arguments for the executables, as string views, and the
// number parsing shared by the three mains.

#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <locale>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace apps {

// The arguments of main(). The C entry point passes them as a raw array:
// the narrow unsafe use at the language boundary (see the modern-cpp
// skill), read here once.
inline std::vector<std::string_view> arguments(int argc, char** argv) {
    const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
    return {raw.begin(), raw.end()};
}

// The value after option @p i of @p args, which @p i then indexes; nothing,
// with @p i unchanged, when the option is the last argument.
inline std::optional<std::string_view> nextValue(std::span<const std::string_view> args, std::size_t& i) {
    if (i + 1 >= args.size()) return std::nullopt;
    return args[++i];
}

// True when @p text is a decimal number: an optional sign, digits with an
// optional fraction, and an optional exponent. No spaces, no hexadecimal,
// no `inf` or `nan`.
inline bool isDecimal(std::string_view text) noexcept {
    std::size_t i = 0;
    const auto digits = [&] {
        const std::size_t start = i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
        return i - start;
    };
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
    std::size_t mantissa = digits();
    if (i < text.size() && text[i] == '.') {
        ++i;
        mantissa += digits();
    }
    if (mantissa == 0) return false;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
        if (digits() == 0) return false;
    }
    return i == text.size();
}

// @p text as a number when it is one and nothing else. An integer type
// takes only integers; a floating-point type takes decimal numbers and
// refuses non-finite values.
template <typename T>
std::optional<T> parseNumber(std::string_view text) {
    T value{};
    if constexpr (std::is_integral_v<T>) {
        // from_chars reads a character range by pointers.
        const char* const first = std::to_address(text.begin());
        const char* const last = std::to_address(text.end());
        const auto [end, error] = std::from_chars(first, last, value);
        if (error != std::errc{} || end != last) return std::nullopt;
    } else {
        if (!isDecimal(text)) return std::nullopt;
        // from_chars for floating point is missing from some standard
        // libraries (lessons.md). One stream per thread, in the classic
        // locale, reads the same in every locale without a stream built
        // for each number.
        thread_local std::istringstream in = [] {
            std::istringstream s;
            s.imbue(std::locale::classic());
            return s;
        }();
        in.clear();
        in.str(std::string(text));
        if (!(in >> value) || !std::isfinite(value)) return std::nullopt;
    }
    return value;
}

// @p text as a number from @p lo to @p hi inclusive, or nothing.
template <typename T>
std::optional<T> parseInRange(std::string_view text, T lo, T hi) {
    const auto value = parseNumber<T>(text);
    // Written as a single "inside" test, so that a NaN would fail it too.
    if (!value || !(*value >= lo && *value <= hi)) return std::nullopt;
    return value;
}

// @p text as a number from 0 to 999,999,999 when it is decimal digits only,
// no sign and no spaces, such as an HTTP Content-Length or a server-sent
// event's retry time; nothing otherwise.
inline std::optional<std::int64_t> parseDigits(std::string_view text) {
    if (!std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
    return parseInRange<std::int64_t>(text, 0, 999'999'999);
}

// @p text as a UDP port from 1 to 65535, or nothing.
inline std::optional<std::uint16_t> parsePort(std::string_view text) {
    const auto port = parseInRange<std::int32_t>(text, 1, 65535);
    if (!port) return std::nullopt;
    return static_cast<std::uint16_t>(*port);
}

} // namespace apps
