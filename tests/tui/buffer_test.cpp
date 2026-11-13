#include <catch2/catch_test_macros.hpp>

#include <proj/tui/buffer.h>

#include <string>

using tui::buffer;
using tui::position;

TEST_CASE("gap buffer inserts and erases anywhere", "[tui][buffer]") {
    tui::gapBuffer g("hello world");
    g.insert(5, ",");
    g.insert(0, ">> ");
    g.erase(3, 5);
    g.insert(g.size(), "!");
    CHECK(g.str() == ">> , world!");
    CHECK(g.substr(5, 5) == "world");
    CHECK(g.at(0) == '>');
    std::string big(10'000, 'x');
    g.insert(4, big);
    CHECK(g.size() == 11 + big.size());
}

TEST_CASE("buffer reports lines and positions in code points", "[tui][buffer]") {
    buffer b("ab\né€x\n");
    CHECK(b.lineCount() == 3);
    CHECK(b.line(1) == "é€x");
    CHECK(b.lineLength(1) == 3);
    CHECK(b.offsetOf({1, 2}) == 3 + 2 + 3);
    CHECK(b.positionOf(3 + 2 + 3) == position{1, 2});
    CHECK(b.clamp({9, 9}) == position{2, 0});
}

TEST_CASE("buffer edits at the cursor and sets dirty", "[tui][buffer]") {
    buffer b("abc");
    CHECK_FALSE(b.dirty());
    b.setCursor({0, 1});
    b.insert("X");
    CHECK(b.text() == "aXbc");
    CHECK(b.dirty());
    b.insertLineBreak();
    CHECK(b.text() == "aX\nbc");
    CHECK(b.cursor() == position{1, 0});
    CHECK(b.deleteBackward());
    CHECK(b.text() == "aXbc");
    CHECK(b.deleteForward());
    CHECK(b.text() == "aXc");
    b.markClean();
    CHECK_FALSE(b.dirty());
}

TEST_CASE("delete removes whole code points", "[tui][buffer]") {
    buffer b("a€b");
    b.setCursor({0, 2});
    CHECK(b.deleteBackward());
    CHECK(b.text() == "ab");
    b.setCursor({0, 0});
    CHECK_FALSE(b.deleteBackward());
}

TEST_CASE("join and kill line", "[tui][buffer]") {
    buffer b("one\ntwo\nthree");
    CHECK(b.joinLines());
    CHECK(b.text() == "onetwo\nthree");
    CHECK(b.cursor() == position{0, 3});
    b.setCursor({0, 3});
    CHECK(b.killLine() == "two");
    CHECK(b.killLine() == "\n");
    CHECK(b.text() == "onethree");
    b.moveBufferEnd();
    CHECK(b.killLine().empty());
    CHECK_FALSE(b.joinLines());
}

TEST_CASE("selection replaces and deletes", "[tui][buffer]") {
    buffer b("hello world");
    b.setCursor({0, 6});
    b.setMark();
    b.moveLineEnd();
    CHECK(b.hasSelection());
    CHECK(b.selectedText() == "world");
    b.insert("there");
    CHECK(b.text() == "hello there");
    CHECK_FALSE(b.mark());
}

TEST_CASE("vertical movement keeps the goal column", "[tui][buffer]") {
    buffer b("abcdef\nab\nabcdef");
    b.setCursor({0, 5});
    b.moveDown();
    CHECK(b.cursor() == position{1, 2});
    b.moveDown();
    CHECK(b.cursor() == position{2, 5});
    b.moveWordBackward();
    CHECK(b.cursor() == position{2, 0});
    b.moveWordBackward();
    CHECK(b.cursor() == position{1, 0});
    b.moveWordForward();
    CHECK(b.cursor() == position{1, 2});
}

TEST_CASE("undo and redo restore text and cursor", "[tui][buffer]") {
    buffer b("abc");
    b.moveLineEnd();
    b.insert("d");
    b.insert("e");  // grouped with "d"
    b.moveLeft();
    b.insert("X");
    CHECK(b.text() == "abcdXe");
    CHECK(b.undoDepth() == 2);

    CHECK(b.undo());
    CHECK(b.text() == "abcde");
    CHECK(b.undo());
    CHECK(b.text() == "abc");
    CHECK(b.cursor() == position{0, 3});
    CHECK_FALSE(b.undo());

    CHECK(b.redo());
    CHECK(b.text() == "abcde");
    CHECK(b.redo());
    CHECK(b.text() == "abcdXe");
    CHECK_FALSE(b.redo());

    CHECK(b.undo());
    b.insert("Y");  // a new edit discards redo history
    CHECK_FALSE(b.canRedo());
}

TEST_CASE("typed characters group up to 20", "[tui][buffer]") {
    buffer b;
    for (int i = 0; i < 25; ++i) b.insert("a");
    CHECK(b.undoDepth() == 2);
    b.insert("\n");
    CHECK(b.undoDepth() == 3);
}

TEST_CASE("undo history is bounded by size (tui.md 8.7)", "[tui][buffer]") {
    buffer b("", {}, tui::undoLimits{100, 150});
    const std::string chunk(40, 'x');
    for (int i = 0; i < 10; ++i) {
        b.moveBufferEnd();
        b.insert("\n" + chunk);
    }
    CHECK(b.undoBytes() <= 150);
    CHECK(b.undoBytes() > 100);  // the change that crosses the soft limit is kept
    CHECK(b.undoDepth() == 3);

    // The most recent change is kept even when it exceeds the hard limit.
    b.insert(std::string(500, 'y'));
    CHECK(b.undoDepth() == 1);
    CHECK(b.undo());
    CHECK(b.text().find('y') == std::string::npos);
}
