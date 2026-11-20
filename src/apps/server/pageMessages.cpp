#include "pageMessages.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <espresso/primitives/io.h>
#include <espresso/protocol/osc.h>

#include "../arguments.h"
#include "httpText.h"
#include "webData.h"

namespace server {

namespace {

using nlohmann::json;

// One message from a page: [address, arguments...].
std::optional<oscBytes> messageFromPage(const json& m, std::string& error) {
    if (!m.is_array() || m.empty() || !m.front().is_string()) {
        error = "each message is an array of an address and its arguments";
        return std::nullopt;
    }
    protocol::message out;
    out.address = m.front().get<std::string>();
    if (!out.address.starts_with('/')) {
        error = "address " + out.address + " does not start with /";
        return std::nullopt;
    }
    for (std::size_t i = 1; i < m.size(); ++i) {
        const json& a = m[i];
        if (a.is_boolean()) {
            out.args.emplace_back(std::int32_t{a.get<bool>() ? 1 : 0});
        } else if (a.is_number()) {
            const auto f = floatOf(a.get<double>());
            if (!f) {
                error = "argument " + std::to_string(i) + " of " + out.address + " does not fit a float";
                return std::nullopt;
            }
            out.args.emplace_back(*f);
        } else if (a.is_string()) {
            out.args.emplace_back(a.get<std::string>());
        } else {
            error = "argument " + std::to_string(i) + " of " + out.address +
                    " is not a number, string, or boolean";
            return std::nullopt;
        }
    }
    auto bytes = protocol::encode(out);
    if (bytes.size() > primitives::maxOscBytes) {
        error = out.address + " is larger than " + std::to_string(primitives::maxOscBytes) + " bytes";
        return std::nullopt;
    }
    return bytes;
}

std::uint32_t big32(std::span<const std::uint8_t> m, std::size_t at) noexcept {
    return (std::uint32_t{m[at]} << 24U) | (std::uint32_t{m[at + 1]} << 16U) |
           (std::uint32_t{m[at + 2]} << 8U) | std::uint32_t{m[at + 3]};
}

std::uint64_t big64(std::span<const std::uint8_t> m, std::size_t at) noexcept {
    return (std::uint64_t{big32(m, at)} << 32U) | big32(m, at + 4);
}

// The string at @p at, and where the next item starts (OSC strings are
// NUL-terminated and padded to four bytes); nothing when it runs off the end.
std::optional<std::pair<std::string, std::size_t>> stringAt(std::span<const std::uint8_t> m, std::size_t at) {
    const auto end = std::find(m.begin() + static_cast<std::ptrdiff_t>(std::min(at, m.size())), m.end(), 0);
    if (end == m.end()) return std::nullopt;
    const auto length = static_cast<std::size_t>(end - m.begin()) - at;
    std::string s(m.begin() + static_cast<std::ptrdiff_t>(at), end);
    return std::pair{std::move(s), at + ((length + 4) & ~std::size_t{3})};
}

// A double as JSON: null when it is not finite, as JSON has no such number.
json number(double d) { return std::isfinite(d) ? json(d) : json(nullptr); }

}  // namespace

std::optional<std::vector<oscBytes>> messagesFromPage(std::string_view text, std::string& error) {
    const json body = json::parse(text, nullptr, false);
    if (body.is_discarded() || !body.is_array()) {
        error = "the body is not a JSON array of messages";
        return std::nullopt;
    }
    std::vector<oscBytes> out;
    out.reserve(body.size());
    for (const json& m : body) {
        auto bytes = messageFromPage(m, error);
        if (!bytes) return std::nullopt;
        out.push_back(std::move(*bytes));
    }
    return out;
}

std::optional<oscBytes> binaryFromPage(std::span<const std::uint8_t> frame) {
    // As a UDP datagram to oscIn: a message of at most 1024 bytes whose
    // address starts with / and ends within it.
    if (frame.empty() || frame.size() > primitives::maxOscBytes || frame.front() != '/') return std::nullopt;
    if (std::ranges::find(frame, 0) == frame.end()) return std::nullopt;
    return oscBytes(frame.begin(), frame.end());
}

std::optional<std::string> messageToPage(std::span<const std::uint8_t> m) {
    const auto address = stringAt(m, 0);
    if (!address || !address->first.starts_with('/')) return std::nullopt;
    json out = json::array({address->first});
    const auto tags = stringAt(m, address->second);
    if (!tags) return out.dump();  // a message without type tags has no arguments
    if (!tags->first.starts_with(',')) return std::nullopt;
    std::size_t at = tags->second;
    const auto need = [&](std::size_t n) { return at + n <= m.size(); };
    for (const char tag : std::string_view(tags->first).substr(1)) {
        switch (tag) {
        case 'i':
            if (!need(4)) return std::nullopt;
            out.push_back(static_cast<std::int32_t>(big32(m, at)));
            at += 4;
            break;
        case 'f':
            if (!need(4)) return std::nullopt;
            out.push_back(number(std::bit_cast<float>(big32(m, at))));
            at += 4;
            break;
        case 'h':
            if (!need(8)) return std::nullopt;
            out.push_back(static_cast<std::int64_t>(big64(m, at)));
            at += 8;
            break;
        case 'd':
            if (!need(8)) return std::nullopt;
            out.push_back(number(std::bit_cast<double>(big64(m, at))));
            at += 8;
            break;
        case 's':
        case 'S': {
            const auto s = stringAt(m, at);
            if (!s) return std::nullopt;
            out.push_back(tag == 's' ? json(s->first) : json(nullptr));
            at = s->second;
            break;
        }
        case 'T': out.push_back(true); break;
        case 'F': out.push_back(false); break;
        case 'N':
        case 'I': out.push_back(nullptr); break;
        case 'c':
        case 'r':
        case 'm':
            if (!need(4)) return std::nullopt;
            out.push_back(nullptr);
            at += 4;
            break;
        case 't':
            if (!need(8)) return std::nullopt;
            out.push_back(nullptr);
            at += 8;
            break;
        case 'b': {
            if (!need(4)) return std::nullopt;
            // In size_t, so a length near 2^32 cannot wrap to a short skip.
            const std::size_t length = big32(m, at);
            at += 4;
            if (length > m.size() - at) return std::nullopt;
            out.push_back(nullptr);
            at += (length + 3) & ~std::size_t{3};
            break;
        }
        default: return std::nullopt;  // the size of an unknown type is unknown
        }
    }
    return out.dump();
}

std::string_view httpMessage::header(std::string_view name) const {
    const auto it = headers.find(name);
    return it == headers.end() ? std::string_view() : std::string_view(it->second);
}

std::string_view httpMessage::path() const { return std::string_view(target).substr(0, target.find('?')); }

std::optional<std::string_view> httpMessage::query(std::string_view name) const {
    const auto q = target.find('?');
    if (q == std::string::npos) return std::nullopt;
    return fieldIn(std::string_view(target).substr(q + 1), '&', name);
}

std::optional<httpMessage> requestReader::next() {
    constexpr std::size_t maxHead = std::size_t{16} << 10U;
    if (error_ != requestError::none) return std::nullopt;
    const auto end = buffer_.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (buffer_.size() > maxHead) error_ = requestError::headersTooLarge;
        return std::nullopt;
    }
    if (end > maxHead) {
        error_ = requestError::headersTooLarge;
        return std::nullopt;
    }
    const std::string_view head = std::string_view(buffer_).substr(0, end);
    httpMessage r;
    const auto lineEnd = head.find("\r\n");
    const std::string_view line = head.substr(0, lineEnd);
    const auto sp1 = line.find(' ');
    const auto sp2 = line.rfind(' ');
    if (sp1 == std::string_view::npos || sp2 == sp1 || line.substr(sp2 + 1) != "HTTP/1.1") {
        error_ = requestError::badRequest;
        return std::nullopt;
    }
    r.method = line.substr(0, sp1);
    r.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (r.target.empty() || r.target.front() != '/') {
        error_ = requestError::badRequest;
        return std::nullopt;
    }
    std::string_view rest = lineEnd == std::string_view::npos ? std::string_view() : head.substr(lineEnd + 2);
    while (!rest.empty()) {
        const auto next = rest.find("\r\n");
        const std::string_view field = rest.substr(0, next);
        const auto colon = field.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            error_ = requestError::badRequest;
            return std::nullopt;
        }
        std::string name = lowered(field.substr(0, colon));
        std::string_view value = field.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
        r.headers.emplace(std::move(name), value);
        if (next == std::string_view::npos) break;
        rest.remove_prefix(next + 2);
    }
    std::size_t length = 0;
    if (const auto it = r.headers.find("content-length"); it != r.headers.end()) {
        // Digits only; more than 9 would be over any body allowed.
        const std::string& v = it->second;
        const auto n = std::ranges::all_of(v, [](char c) { return c >= '0' && c <= '9'; })
                           ? apps::parseInRange<std::int64_t>(v, 0, 999'999'999)
                           : std::nullopt;
        if (!n) {
            error_ = requestError::badRequest;
            return std::nullopt;
        }
        length = static_cast<std::size_t>(*n);
    } else if (r.headers.contains("transfer-encoding") || r.method == "POST" || r.method == "PUT") {
        error_ = requestError::lengthRequired;  // only Content-Length bodies (§12.11)
        return std::nullopt;
    }
    if (length > maxPageBody) {
        error_ = requestError::tooLarge;
        return std::nullopt;
    }
    if (buffer_.size() < end + 4 + length) return std::nullopt;
    r.body = buffer_.substr(end + 4, length);
    buffer_.erase(0, end + 4 + length);
    return r;
}

}  // namespace server
