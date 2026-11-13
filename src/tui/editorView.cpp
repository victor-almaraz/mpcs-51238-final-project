#include <proj/tui/editorView.h>

#include <algorithm>
#include <string>

#include <proj/tui/utf8.h>

namespace tui {

using namespace ftxui;

Decorator styleFor(highlightCategory category) {
    switch (category) {
    case highlightCategory::comment: return dim;
    case highlightCategory::string: return color(Color::Green);
    case highlightCategory::number: return color(Color::Yellow);
    case highlightCategory::unitLiteral: return color(Color::Yellow) | bold;
    case highlightCategory::keyword: return color(Color::Cyan);
    case highlightCategory::specialForm: return color(Color::Magenta) | bold;
    case highlightCategory::objectForm: return color(Color::Magenta);
    case highlightCategory::symbol: return nothing;
    case highlightCategory::delimiter: return dim;
    case highlightCategory::error: return color(Color::Red) | underlined;
    }
    return nothing;
}

int gutterWidth(const buffer& b) {
    return static_cast<int>(std::to_string(b.lineCount()).size()) + 1;
}

namespace {

// Display width of the code point `cp` starting at display column `col`.
std::size_t cellWidth(std::string_view cp, std::size_t col) {
    return cp == "\t" ? tabWidth - col % tabWidth : 1;
}

struct runStyle {
    highlightCategory category = highlightCategory::symbol;
    bool selected = false;
    bool cursor = false;
    bool paren = false;
    friend bool operator==(const runStyle&, const runStyle&) = default;
};

Element styled(std::string text, const runStyle& s) {
    Element e = ftxui::text(std::move(text)) | styleFor(s.category);
    if (s.paren) e = e | bold | bgcolor(Color::Blue);
    if (s.selected) e = e | bgcolor(Color::GrayDark);
    if (s.cursor) e = e | inverted;
    return e;
}

// Offsets of the cursor's delimiter and its match, if any.
std::pair<std::optional<std::size_t>, std::optional<std::size_t>>
delimiterPair(const buffer& b, const highlightResult& h) {
    const std::size_t at = b.cursorOffset();
    if (auto m = matchingDelimiter(h.tokens, at)) return {at, m};
    if (at > 0) {
        if (auto m = matchingDelimiter(h.tokens, at - 1)) {
            // Only closers before the cursor, as in Emacs show-paren-mode.
            if (*m < at - 1) return {at - 1, m};
        }
    }
    return {std::nullopt, std::nullopt};
}

} // namespace

std::size_t displayColumn(const buffer& b, position pos) {
    pos = b.clamp(pos);
    const std::string line = b.line(pos.line);
    std::size_t col = 0;
    std::size_t i = 0;
    for (std::size_t cp = 0; cp < pos.column && i < line.size(); ++cp) {
        const std::size_t len = utf8::sequenceLength(static_cast<unsigned char>(line[i]));
        col += cellWidth(std::string_view(line).substr(i, len), col);
        i += len;
    }
    return col;
}

void ensureCursorVisible(buffer& b, int rows, int columns) {
    b.scrollToCursor(static_cast<std::size_t>(std::max(rows, 1)));
    const std::size_t width = static_cast<std::size_t>(std::max(columns, 1));
    const std::size_t col = displayColumn(b, b.cursor());
    if (col < b.leftColumn()) b.setLeftColumn(col);
    if (col >= b.leftColumn() + width) b.setLeftColumn(col - width + 1);
}

Element renderEditor(const buffer& b, const highlightResult& h, int rows, int columns,
                     editorViewOptions options) {
    const int gutter = options.showGutter ? gutterWidth(b) : 0;
    const std::size_t width = static_cast<std::size_t>(std::max(columns - gutter, 1));
    const std::size_t left = b.leftColumn();
    const auto [selFirst, selLast] = b.hasSelection() ? b.selectionRange()
                                                      : std::pair<std::size_t, std::size_t>{0, 0};
    const auto [parenA, parenB] = delimiterPair(b, h);
    const position cursor = b.cursor();
    const auto& spans = h.spans;

    Elements lines;
    for (int row = 0; row < rows; ++row) {
        const std::size_t lineIndex = b.topLine() + static_cast<std::size_t>(row);
        if (lineIndex >= b.lineCount()) {
            lines.push_back(text(""));
            continue;
        }
        Elements cells;
        if (options.showGutter) {
            std::string number = std::to_string(lineIndex + 1);
            number.insert(0, static_cast<std::size_t>(gutter - 1) - number.size(), ' ');
            cells.push_back(text(number + " ") | dim);
        }

        const std::string line = b.line(lineIndex);
        const std::size_t lineStart = b.offsetOf({lineIndex, 0});
        auto span = std::lower_bound(spans.begin(), spans.end(), lineStart,
                                     [](const highlightSpan& s, std::size_t off) {
                                         return s.offset + s.length <= off;
                                     });
        std::string run;
        runStyle current;
        auto flush = [&] {
            if (!run.empty()) cells.push_back(styled(std::move(run), current));
            run.clear();
        };

        std::size_t col = 0;
        std::size_t cpIndex = 0;
        for (std::size_t i = 0; i < line.size() && col < left + width; ++cpIndex) {
            const std::size_t len = utf8::sequenceLength(static_cast<unsigned char>(line[i]));
            const std::string_view cp = std::string_view(line).substr(i, len);
            const std::size_t abs = lineStart + i;
            while (span != spans.end() && span->offset + span->length <= abs) ++span;

            runStyle style;
            if (span != spans.end() && span->offset <= abs) style.category = span->category;
            style.selected = abs >= selFirst && abs < selLast;
            style.cursor = options.showCursor && lineIndex == cursor.line && cpIndex == cursor.column;
            style.paren = abs == parenA || abs == parenB;

            const std::size_t w = cellWidth(cp, col);
            for (std::size_t k = 0; k < w; ++k, ++col) {
                if (col < left || col >= left + width) continue;
                if (!(style == current)) {
                    flush();
                    current = style;
                }
                run += cp == "\t" ? std::string_view(" ") : cp;
                if (cp == "\t") style.cursor = false;  // cursor shows on the first cell only
            }
            i += len;
        }
        flush();
        if (options.showCursor && lineIndex == cursor.line && cpIndex <= cursor.column &&
            col >= left && col < left + width) {
            cells.push_back(text(" ") | inverted);
        }
        cells.push_back(filler());
        lines.push_back(hbox(std::move(cells)));
    }
    return vbox(std::move(lines));
}

position hitTest(const buffer& b, int row, int column, bool showGutter) {
    const std::size_t lineIndex =
        std::min(b.topLine() + static_cast<std::size_t>(std::max(row, 0)), b.lineCount() - 1);
    const int gutter = showGutter ? gutterWidth(b) : 0;
    const std::size_t target = b.leftColumn() + static_cast<std::size_t>(std::max(column - gutter, 0));
    const std::string line = b.line(lineIndex);
    std::size_t col = 0;
    std::size_t cpIndex = 0;
    for (std::size_t i = 0; i < line.size(); ++cpIndex) {
        const std::size_t len = utf8::sequenceLength(static_cast<unsigned char>(line[i]));
        const std::size_t w = cellWidth(std::string_view(line).substr(i, len), col);
        if (target < col + w) return {lineIndex, cpIndex};
        col += w;
        i += len;
    }
    return {lineIndex, cpIndex};
}

} // namespace tui
