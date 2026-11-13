#include <catch2/catch_test_macros.hpp>

#include <proj/tui/minibuffer.h>

#include <string>
#include <vector>

using namespace tui;

TEST_CASE("minibuffer edits by code point", "[tui][minibuffer]") {
    minibuffer m;
    m.open("Open file: ", "ab");
    CHECK(m.active());
    CHECK(m.cursor() == 2);
    m.moveLeft();
    m.insert("é");
    CHECK(m.text() == "aéb");
    m.deleteBackward();
    CHECK(m.text() == "ab");
    m.moveStart();
    m.deleteForward();
    CHECK(m.text() == "b");
    m.close();
    CHECK_FALSE(m.active());
}

TEST_CASE("completion extends to the common prefix (tui.md 8.6)", "[tui][minibuffer]") {
    minibuffer m;
    m.open("Open file: ", "v", {"main.lisp", "voices.lisp", "voices-old.lisp", "util/v.lisp"});
    CHECK(m.complete());
    CHECK(m.text() == "voices");
    CHECK_FALSE(m.complete());
    CHECK(m.matches() == std::vector<std::string>{"voices.lisp", "voices-old.lisp"});
    m.setText("lisp");
    CHECK(m.matches().size() == 4);
    CHECK(commonPrefix({"é1", "é2"}) == "é");
    CHECK(commonPrefix({}).empty());
}
