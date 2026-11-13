/// @file
/// Rendering of a buffer as an FTXUI element, and mapping of screen
/// cells back to buffer positions (`tui.md` §6.4, §4.1).

#pragma once

#include <cstddef>
#include <optional>

#include <ftxui/dom/elements.hpp>

#include <proj/tui/buffer.h>
#include <proj/tui/highlighter.h>
#include <proj/tui/theme.h>

namespace tui {

/// Display width of a tab stop.
inline constexpr std::size_t tabWidth = 8;

/// Style for a highlight category in @p colors (`tui.md` §6.2).
ftxui::Decorator styleFor(highlightCategory category, const theme& colors = defaultTheme());

/// Width of the line-number gutter for @p b, including its padding.
int gutterWidth(const buffer& b);

/// Display column of position @p pos, with tabs expanded.
std::size_t displayColumn(const buffer& b, position pos);

/// Scrolls @p b so that the cursor is inside a text area of @p rows
/// rows and @p columns columns (excluding the gutter).
void ensureCursorVisible(buffer& b, int rows, int columns);

/// What the view draws besides text.
struct editorViewOptions {
    /// Draw the cursor.
    bool showCursor = true;
    /// Draw line numbers.
    bool showGutter = true;
    const theme* colors = nullptr;  ///< Null: defaultTheme()
};

/// Renders the visible lines of @p b with @p highlights, which must be
/// computed from the buffer's current text. Only visible lines are
/// built (§6.4). The cursor's matching delimiter is emphasized (§6.5).
ftxui::Element renderEditor(const buffer& b, const highlightResult& highlights, int rows,
                            int columns, editorViewOptions options = {});

/// Buffer position under the cell at @p row and @p column, relative to
/// the view's top-left corner.
position hitTest(const buffer& b, int row, int column, bool showGutter = true);

} // namespace tui
