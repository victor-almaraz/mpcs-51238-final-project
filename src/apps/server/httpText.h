// Text rules shared by the web and page bridges: lower case for names
// that compare without case, percent-encoding (RFC 3986), and the fields
// of a delimited list such as a query string or a Cookie header.

#pragma once

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace server {

/// @p s in lower case, byte by byte (ASCII).
inline std::string lowered(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// @p s percent-encoded for a query string: every byte but the unreserved
/// ones.
inline std::string percentEncoded(std::string_view s) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string out;
    for (const char c : s) {
        const auto b = static_cast<unsigned char>(c);
        if (std::isalnum(b) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += hex[b >> 4U];
            out += hex[b & 0xFU];
        }
    }
    return out;
}

/// @p s with each `%XX` decoded; nothing when an escape is malformed.
inline std::optional<std::string> percentDecoded(std::string_view s) {
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out += s[i];
            continue;
        }
        if (i + 2 >= s.size()) return std::nullopt;
        const int high = digit(s[i + 1]);
        const int low = digit(s[i + 2]);
        if (high < 0 || low < 0) return std::nullopt;
        out += static_cast<char>((static_cast<unsigned>(high) << 4U) | static_cast<unsigned>(low));
        i += 2;
    }
    return out;
}

/// The value of field @p name in @p list, fields `name=value` separated by
/// @p separator, spaces before a name skipped: `a=1&b=2` or `a=1; b=2`. A
/// field without `=` has the empty value; nothing when @p name is absent.
inline std::optional<std::string_view> fieldIn(std::string_view list, char separator, std::string_view name) {
    while (!list.empty()) {
        const auto end = list.find(separator);
        std::string_view field = list.substr(0, end);
        while (!field.empty() && field.front() == ' ') field.remove_prefix(1);
        const auto eq = field.find('=');
        if (field.substr(0, eq) == name)
            return eq == std::string_view::npos ? std::string_view() : field.substr(eq + 1);
        if (end == std::string_view::npos) break;
        list.remove_prefix(end + 1);
    }
    return std::nullopt;
}

}  // namespace server
