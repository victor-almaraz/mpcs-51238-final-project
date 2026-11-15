#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <proj/tui/buffer.h>
#include <proj/tui/editorView.h>
#include <proj/tui/highlighter.h>

using namespace tui;

namespace {

ftxui::Screen draw(const buffer& b, int width, int height, editorViewOptions options = {}) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, renderEditor(b, highlight(b.text()), height, width, options));
    return screen;
}

std::vector<std::string> rows(const ftxui::Screen& s) {
    std::vector<std::string> out;
    for (int y = 0; y < s.dimy(); ++y) {
        std::string row;
        for (int x = 0; x < s.dimx(); ++x) {
            const std::string& c = s.CellAt(x, y).character;
            row += c.empty() ? " " : c;
        }
        while (!row.empty() && row.back() == ' ') row.pop_back();
        out.push_back(row);
    }
    return out;
}

} // namespace

TEST_CASE("editor renders visible lines with a gutter (tui.md 10.4)", "[tui][editorView]") {
    buffer b("(play main)\n; note\n\tx");
    const auto screen = draw(b, 24, 4);
    CHECK(rows(screen) == std::vector<std::string>{"1 (play main)", "2 ; note", "3         x", ""});
}

TEST_CASE("editor applies highlight styles and the cursor", "[tui][editorView]") {
    buffer b("(define x \"s\")");
    b.setCursor({.line = 0, .column = 1});
    const auto screen = draw(b, 30, 1, {.showCursor = true, .showGutter = false, .colors = std::nullopt});
    CHECK(screen.CellAt(1, 0).inverted);       // cursor on "d"
    CHECK(screen.CellAt(2, 0).bold);           // special form
    CHECK(screen.CellAt(0, 0).dim);            // delimiter
    CHECK_FALSE(screen.CellAt(8, 0).bold);     // plain symbol
}

TEST_CASE("editor emphasizes the matching delimiter (tui.md 6.5)", "[tui][editorView]") {
    buffer b("(a (b) c)");
    b.setCursor({.line = 0, .column = 0});
    const auto screen = draw(b, 20, 1, {.showCursor = true, .showGutter = false, .colors = std::nullopt});
    CHECK(screen.CellAt(8, 0).bold);
    CHECK_FALSE(screen.CellAt(5, 0).bold);
}

TEST_CASE("editor scrolls to keep the cursor visible", "[tui][editorView]") {
    std::string text;
    for (int i = 1; i <= 50; ++i) text += "line " + std::to_string(i) + "\n";
    buffer b(text);
    b.setCursor({.line = 40, .column = 0});
    ensureCursorVisible(b, 10, 20);
    CHECK(b.topLine() == 31);
    CHECK(rows(draw(b, 20, 10, {false, false, std::nullopt})).back() == "line 41");

    buffer wide(std::string(100, 'x') + "y");
    wide.moveLineEnd();
    ensureCursorVisible(wide, 1, 10);
    CHECK(wide.leftColumn() == 92);
}

TEST_CASE("hit testing maps cells to positions", "[tui][editorView]") {
    buffer b("ab\n\tc\nlong line");
    CHECK(hitTest(b, 0, 1, false) == position{0, 1});
    CHECK(hitTest(b, 1, 5, false) == position{1, 0});   // inside the tab
    CHECK(hitTest(b, 1, 8, false) == position{1, 1});
    CHECK(hitTest(b, 0, 50, false) == position{0, 2});  // past the end of the line
    CHECK(hitTest(b, 9, 0, false) == position{2, 0});   // below the last line
    CHECK(hitTest(b, 0, 3, true) == position{0, 1});    // gutter is 2 columns wide
}

TEST_CASE("formBefore finds the last complete form", "[tui][highlighter]") {
    const std::string_view src = "(a) '(b (c)) ; x\n d";
    const auto tokens = highlight(src).tokens;
    const auto form = [&](std::size_t at) {
        const auto r = formBefore(tokens, at);
        return r ? std::string(src.substr(r->first, r->second - r->first)) : std::string("<none>");
    };
    CHECK(form(3) == "(a)");
    CHECK(form(12) == "'(b (c))");
    CHECK(form(16) == "'(b (c))");
    CHECK(form(src.size()) == "d");
    CHECK(form(0) == "<none>");
    CHECK(form(6) == "<none>");  // directly after an opening delimiter
}
