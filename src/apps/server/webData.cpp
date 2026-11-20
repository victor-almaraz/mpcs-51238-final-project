#include "webData.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/io.h>

#include "serialLines.h"

namespace server {

namespace {

using nlohmann::json;

// The number @p v holds (io.md §8.2), or nothing.
std::optional<float> numberIn(const json& v) {
    if (v.is_boolean()) return v.get<bool>() ? 1.0f : 0.0f;
    if (v.is_string()) return parseNumber(v.get_ref<const std::string&>());
    if (!v.is_number()) return std::nullopt;
    const double d = v.get<double>();
    if (!std::isfinite(d) || std::abs(d) > std::numeric_limits<float>::max()) return std::nullopt;
    return static_cast<float>(d);
}

// The value at @p path in @p doc, or nothing when the path is absent or
// not a JSON Pointer.
std::optional<float> numberAt(const json& doc, const std::string& path) {
    try {
        const json::json_pointer pointer(path);
        if (!doc.contains(pointer)) return std::nullopt;
        return numberIn(doc.at(pointer));
    } catch (const json::exception&) {
        return std::nullopt;  // not a pointer, or an array index that is not a number
    }
}

}  // namespace

document parseDocument(std::string_view text) {
    if (text.size() > maxDocumentBytes) return {.kind = documentKind::tooLarge, .json = {}};
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded()) return {.kind = documentKind::notJson, .json = {}};
    return {.kind = documentKind::read, .json = std::move(doc)};
}

extraction fieldsOf(const json& doc, std::span<const std::string> fields) {
    extraction out;
    static const std::array<std::string, 1> whole{""};
    const auto paths = fields.empty() ? std::span<const std::string>(whole) : fields;
    const auto count = std::min(paths.size(), static_cast<std::size_t>(metadata::maxChannels));
    for (std::size_t i = 0; i < count; ++i) {
        if (const auto v = numberAt(doc, paths[i])) {
            out.frame.values[i] = *v;
        } else {
            out.frame.missing = static_cast<std::uint8_t>(out.frame.missing | (1U << i));
            ++out.missing;
        }
    }
    out.frame.count = static_cast<std::uint8_t>(count);
    return out;
}

extraction extractFields(std::string_view text, std::span<const std::string> fields) {
    const document doc = parseDocument(text);
    if (doc.kind != documentKind::read) {
        extraction refused;
        refused.kind = doc.kind;
        return refused;
    }
    return fieldsOf(doc.json, fields);
}

void sseParser::reset() {
    line_.clear();
    afterCr_ = false;
    lineTooLong_ = false;
    bomAt_ = 0;
    dropping_ = false;
    event_.clear();
    data_.clear();
}

void sseParser::feed(std::string_view bytes, const std::function<void(const sseMessage&)>& message) {
    // A UTF-8 byte-order mark may open the stream; it is not data.
    constexpr std::string_view bom = "\xEF\xBB\xBF";
    for (const char c : bytes) {
        if (bomAt_ < bom.size()) {
            if (c == bom[bomAt_]) {
                ++bomAt_;
                continue;
            }
            const std::size_t held = std::exchange(bomAt_, bom.size());
            for (const char b : bom.substr(0, held)) take(b, message);
        }
        take(c, message);
    }
}

void sseParser::take(char c, const std::function<void(const sseMessage&)>& message) {
    if (c == '\n' && afterCr_) {  // the LF of a CRLF
        afterCr_ = false;
        return;
    }
    afterCr_ = c == '\r';
    if (c != '\r' && c != '\n') {
        if (line_.size() < maxDocumentBytes) {
            line_.push_back(c);
        } else {
            lineTooLong_ = true;
        }
        return;
    }
    if (lineTooLong_) {
        dropping_ = true;  // the message it belongs to is dropped whole
    } else {
        line(line_, message);
    }
    line_.clear();
    lineTooLong_ = false;
}

void sseParser::line(std::string_view text, const std::function<void(const sseMessage&)>& message) {
    if (text.empty()) {
        if (dropping_) {
            ++tooLarge_;
        } else if (!data_.empty()) {
            data_.pop_back();  // the line feed after the last data line
            message({.event = event_.empty() ? std::string("message") : event_,
                     .data = data_,
                     .lastEventId = lastEventId_});
        }
        event_.clear();
        data_.clear();
        dropping_ = false;
        return;
    }
    if (text.front() == ':') return;  // a comment
    const auto colon = text.find(':');
    const std::string_view field = text.substr(0, colon);
    std::string_view value = colon == std::string_view::npos ? std::string_view() : text.substr(colon + 1);
    if (value.starts_with(' ')) value.remove_prefix(1);
    if (field == "event") {
        event_ = value;
    } else if (field == "data") {
        if (data_.size() + value.size() + 1 > maxDocumentBytes) {
            dropping_ = true;
            return;
        }
        data_ += value;
        data_ += '\n';
    } else if (field == "id") {
        if (value.find('\0') == std::string_view::npos) lastEventId_ = value;
    } else if (field == "retry" && !value.empty() && value.size() <= 9 &&
               std::ranges::all_of(value, [](char d) { return d >= '0' && d <= '9'; })) {
        // Digits only; more than 9 would not fit a delay worth waiting.
        retry_ = std::chrono::milliseconds(std::stol(std::string(value)));
    }
}

std::chrono::milliseconds reconnectDelay(unsigned failures, std::chrono::milliseconds retry) noexcept {
    const auto base = retry.count() > 0 ? retry : initialReconnect;
    const auto ceiling = std::max(base, longestReconnect);
    auto delay = base;
    for (unsigned k = 1; k < failures && delay < ceiling; ++k) delay *= 2;
    return std::min(delay, ceiling);
}

}  // namespace server
