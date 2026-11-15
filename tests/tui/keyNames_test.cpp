#include <catch2/catch_test_macros.hpp>

#include <proj/tui/keyNames.h>

#include <string>

using tui::keyNameFromInput;

TEST_CASE("control bytes map to Emacs key names", "[tui][keyNames]") {
    CHECK(keyNameFromInput("\x18") == "C-x");
    CHECK(keyNameFromInput("\x13") == "C-s");
    CHECK(keyNameFromInput(std::string(1, '\0')) == "C-SPC");
    CHECK(keyNameFromInput("\x1f") == "C-_");
    CHECK(keyNameFromInput("\x7f") == "DEL");
    CHECK(keyNameFromInput("\n") == "RET");
    CHECK(keyNameFromInput("\r") == "RET");
    CHECK(keyNameFromInput("\t") == "TAB");
    CHECK(keyNameFromInput("\x1b") == "ESC");
    CHECK(keyNameFromInput(" ") == "SPC");
}

TEST_CASE("printable characters are their own names", "[tui][keyNames]") {
    CHECK(keyNameFromInput("a") == "a");
    CHECK(keyNameFromInput("(") == "(");
    CHECK(keyNameFromInput("é") == "é");
}

TEST_CASE("escape prefixes are Meta", "[tui][keyNames]") {
    CHECK(keyNameFromInput("\x1b" "f") == "M-f");
    CHECK(keyNameFromInput("\x1b" "<") == "M-<");
    CHECK(keyNameFromInput("\x1b\x1f") == "C-M-_");
    CHECK(keyNameFromInput("\x1b" " ") == "M-SPC");
}

TEST_CASE("CSI sequences map to named keys with modifiers", "[tui][keyNames]") {
    CHECK(keyNameFromInput("\x1b[A") == "<up>");
    CHECK(keyNameFromInput("\x1bOD") == "<left>");
    CHECK(keyNameFromInput("\x1b[1;2C") == "S-<right>");
    CHECK(keyNameFromInput("\x1b[1;5D") == "C-<left>");
    CHECK(keyNameFromInput("\x1b[1;6H") == "C-S-<home>");
    CHECK(keyNameFromInput("\x1b[3~") == "<delete>");
    CHECK(keyNameFromInput("\x1b[5~") == "<prior>");
    CHECK(keyNameFromInput("\x1b[6;5~") == "C-<next>");
    CHECK(keyNameFromInput("\x1b[15~") == "<f5>");
    CHECK(keyNameFromInput("\x1b[24~") == "<f12>");
    CHECK(keyNameFromInput("\x1bOR") == "<f3>");
    CHECK(keyNameFromInput("\x1b[1;2R") == "S-<f3>");
    CHECK(keyNameFromInput("\x1b[Z") == "S-TAB");
}

TEST_CASE("non-key input has no name", "[tui][keyNames]") {
    CHECK(keyNameFromInput("").empty());
    CHECK(keyNameFromInput("\x1b[<0;10;5M").empty());
    CHECK(keyNameFromInput("\x1b[99~").empty());
    CHECK(keyNameFromInput("\xff").empty());
}

TEST_CASE("oversized and zero CSI parameters are handled", "[tui][keyNames]") {
    CHECK(keyNameFromInput("\x1b[99999999999~").empty());  // no signed overflow
    CHECK(keyNameFromInput("\x1b[1000~").empty());
    CHECK(keyNameFromInput("\x1b[1;0A") == "<up>");  // modifier 0 means none
    CHECK(keyNameFromInput("\x1b[1;5A") == "C-<up>");
}
