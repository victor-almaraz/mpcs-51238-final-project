#include <proj/tui/editorView.h>

#include <algorithm>
#include <string>

#include <proj/tui/utf8.h>

namespace tui {

using namespace ftxui;

Decorator styleFor(highlightCategory category, const theme& colors) {
    const syntaxStyle& st = colors.styleOf(category);
    Decorator d = color(st.fg);
    if (st.bold) d = d | bold;
    if (st.faint) d = d | dim;
    if (st.underline) d = d | underlined;
    return d;
}

int gutterWidth(const buffer& b) {
    return static_cast<int>(std::to_string(b.lineCount()).size()) + 1;
}

namespace {

// A code point of a line and where it lands on screen.
struct cell {
    std::string_view cp;
    std::size_t byte;    // offset in the line
    std::size_t index;   // code point index (the buffer's column)
    std::size_t column;  // display column, tabs expanded
    std::size_t width;   // display cells it covers
};

// Visits the code points of `line` in order until `visit` returns false.
// Returns the code point index and display column where the walk
// stopped, or the line's end. Every column computation goes through
// here, so tab expansion has one definition.
template <typename F>
std::pair<std::size_t, std::size_t> walkCells(std::string_view line, F&& visit) {
    std::size_t index = 0;
    std::size_t column = 0;
    for (std::size_t i = 0; i < line.size(); ++index) {
        const std::size_t len = utf8::sequenceLength(static_cast<unsigned char>(line[i]));
        const std::string_view cp = line.substr(i, len);
        const std::size_t width = cp == "\t" ? tabWidth - column % tabWidth : 1;
        if (!visit(cell{cp, i, index, column, width})) break;
        column += width;
        i += len;
    }
    return {index, column};
}

struct runStyle {
    highlightCategory category = highlightCategory::symbol;
    bool selected = false;
    bool cursor = false;
    bool paren = false;
    friend bool operator==(const runStyle&, const runStyle&) = default;
};

Element styled(std::string text, const runStyle& s, const theme& t) {
    Element e = ftxui::text(std::move(text)) | styleFor(s.category, t);
    if (s.paren) e = e | bold | bgcolor(t.matchParen);
    if (s.selected) e = e | bgcolor(t.editorSelection);
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
    return walkCells(b.line(pos.line), [&](const cell& c) { return c.index < pos.column; }).second;
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
    const theme& t = options.colors ? options.colors->get() : defaultTheme();

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
            Element gutterText = text(number + " ");
            cells.push_back(t.faintSecondary ? gutterText | dim : gutterText | color(t.secondary));
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
            if (!run.empty()) cells.push_back(styled(std::move(run), current, t));
            run.clear();
        };

        const auto [cpIndex, col] = walkCells(line, [&](const cell& c) {
            if (c.column >= left + width) return false;
            const std::size_t abs = lineStart + c.byte;
            while (span != spans.end() && span->offset + span->length <= abs) ++span;

            runStyle style;
            if (span != spans.end() && span->offset <= abs) style.category = span->category;
            style.selected = abs >= selFirst && abs < selLast;
            style.cursor = options.showCursor && lineIndex == cursor.line && c.index == cursor.column;
            style.paren = abs == parenA || abs == parenB;

            for (std::size_t k = 0; k < c.width; ++k) {
                const std::size_t at = c.column + k;
                if (at < left || at >= left + width) continue;
                if (!(style == current)) {
                    flush();
                    current = style;
                }
                run += c.cp == "\t" ? std::string_view(" ") : c.cp;
                if (c.cp == "\t") style.cursor = false;  // cursor shows on the first cell only
            }
            return true;
        });
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
    const std::size_t index =
        walkCells(b.line(lineIndex), [&](const cell& c) { return target >= c.column + c.width; })
            .first;
    return {lineIndex, index};
}

} // namespace tui
