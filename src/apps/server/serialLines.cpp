#include "serialLines.h"

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
#include <utility>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/io.h>

#include "../arguments.h"

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

}  // namespace

std::optional<float> parseNumber(std::string_view field) {
    // The decimal grammar of the command line (arguments.h), in the range
    // of a float.
    constexpr double largest = std::numeric_limits<float>::max();
    const auto v = apps::parseInRange<double>(field, -largest, largest);
    if (!v) return std::nullopt;
    return static_cast<float>(*v);
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
    // The fewest significant digits that read back to the same float,
    // found by halving: more digits never read back worse, and 9 always do.
    // Floating-point std::to_chars is missing on the macOS deployment
    // target (lessons.md), so one stream per thread, in the classic
    // locale, writes them.
    thread_local std::ostringstream out = [] {
        std::ostringstream s;
        s.imbue(std::locale::classic());
        return s;
    }();
    const auto format = [&](int digits) {
        out.str({});
        out.clear();
        out << std::setprecision(digits) << v;
        return out.str();
    };
    int low = 1;
    int high = std::numeric_limits<float>::max_digits10;
    std::string shortest = format(high);
    while (low < high) {
        const int middle = low + (high - low) / 2;
        std::string text = format(middle);
        if (parseNumber(text) == v) {
            high = middle;
            shortest = std::move(text);
        } else {
            low = middle + 1;
        }
    }
    return shortest;
}

std::string formatLine(const primitives::dataFrame& frame) {
    std::string out;
    for (const float v : frame.used()) {
        if (!out.empty()) out += ' ';
        out += formatNumber(v);
    }
    out += '\n';
    return out;
}

}  // namespace server
