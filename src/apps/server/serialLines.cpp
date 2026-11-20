#include "serialLines.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/io.h>

namespace server {

void lineSplitter::feed(std::span<const char> bytes, const std::function<void(std::string_view)>& line) {
    for (const char c : bytes) {
        if (c == '\n') {
            std::string_view text(pending_);
            if (text.ends_with('\r')) text.remove_suffix(1);
            if (skipping_ || text.size() > maxLineBytes) {
                ++tooLong_;
            } else {
                line(text);
            }
            pending_.clear();
            skipping_ = false;
        } else if (!skipping_) {
            // Held up to one byte past the limit, for a carriage return;
            // past that the line is too long whatever follows.
            if (pending_.size() == maxLineBytes + 1) {
                skipping_ = true;
                pending_.clear();
            } else {
                pending_.push_back(c);
            }
        }
    }
}

namespace {

bool isSeparator(char c) noexcept { return c == ' ' || c == '\t' || c == ','; }
bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

// True when @p s is a decimal number: an optional sign, digits with an
// optional point and at least one digit, then an optional exponent.
bool isDecimal(std::string_view s) noexcept {
    std::size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    const auto skipDigits = [&] {
        const std::size_t start = i;
        while (i < s.size() && isDigit(s[i])) ++i;
        return i - start;
    };
    std::size_t digits = skipDigits();
    if (i < s.size() && s[i] == '.') {
        ++i;
        digits += skipDigits();
    }
    if (digits == 0) return false;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        if (skipDigits() == 0) return false;
    }
    return i == s.size();
}

}  // namespace

std::optional<float> parseNumber(std::string_view field) {
    if (!isDecimal(field)) return std::nullopt;
    // A stream in the classic locale reads the same in every locale;
    // floating-point std::from_chars is missing on macOS (lessons.md).
    std::istringstream in{std::string(field)};
    in.imbue(std::locale::classic());
    double v = 0.0;
    in >> v;
    if (in.fail() || !std::isfinite(v) || std::abs(v) > std::numeric_limits<float>::max())
        return std::nullopt;
    return static_cast<float>(v);
}

parsedLine parseLine(std::string_view line) {
    parsedLine out;
    std::size_t at = 0;
    while (true) {
        while (at < line.size() && isSeparator(line[at])) ++at;
        if (at == line.size()) break;
        std::size_t end = at;
        while (end < line.size() && !isSeparator(line[end])) ++end;
        const auto number = parseNumber(line.substr(at, end - at));
        if (!number || out.frame.count == metadata::maxChannels)
            return {.kind = lineKind::malformed, .frame = {}};
        out.frame.values[out.frame.count++] = *number;
        at = end;
    }
    out.kind = out.frame.count == 0 ? lineKind::empty : lineKind::frame;
    return out;
}

std::string formatNumber(float v) {
    // The fewest significant digits that read back to the same float; 9
    // always do. Floating-point std::to_chars is missing on the macOS
    // deployment target (lessons.md).
    std::string text;
    for (int digits = 1; digits <= std::numeric_limits<float>::max_digits10; ++digits) {
        std::ostringstream s;
        s.imbue(std::locale::classic());
        s << std::setprecision(digits) << v;
        text = s.str();
        if (parseNumber(text) == v) break;
    }
    return text;
}

std::string formatLine(const primitives::dataFrame& frame) {
    std::string out;
    const auto count = std::min<std::size_t>(frame.count, frame.values.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) out += ' ';
        out += formatNumber(frame.values[i]);
    }
    out += '\n';
    return out;
}

}  // namespace server
