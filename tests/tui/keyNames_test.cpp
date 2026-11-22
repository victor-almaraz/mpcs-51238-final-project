#include <catch2/catch_test_macros.hpp>

#include <string>

#include <espresso/tui/keyNames.h>

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

TEST_CASE("control bytes 0x1C to 0x1E are Ctrl and punctuation", "[tui][keyNames]") {
    CHECK(keyNameFromInput("\x1c") == "C-\\");
    CHECK(keyNameFromInput("\x1d") == "C-]");
    CHECK(keyNameFromInput("\x1e") == "C-^");
    CHECK(keyNameFromInput("\x1b\x1d") == "C-M-]");
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

TEST_CASE("xterm cursor and F1 to F4 keys have their names", "[tui][keyNames]") {
    // CSI or SS3 and a final letter; modifiers as CSI 1 ; m and the letter.
    CHECK(keyNameFromInput("\x1b[B") == "<down>");
    CHECK(keyNameFromInput("\x1bOB") == "<down>");
    CHECK(keyNameFromInput("\x1b[1;2B") == "S-<down>");
    CHECK(keyNameFromInput("\x1b[F") == "<end>");
    CHECK(keyNameFromInput("\x1b[1;5F") == "C-<end>");
    CHECK(keyNameFromInput("\x1bOP") == "<f1>");
    CHECK(keyNameFromInput("\x1bOQ") == "<f2>");
    CHECK(keyNameFromInput("\x1bOS") == "<f4>");
    CHECK(keyNameFromInput("\x1b[1;3P") == "M-<f1>");
    CHECK(keyNameFromInput("\x1b[1;7S") == "C-M-<f4>");
}

TEST_CASE("tilde sequences of VT220 terminals and rxvt have their names", "[tui][keyNames]") {
    // Home and End are 1 and 4 (VT220, the Linux console) or 7 and 8 (rxvt);
    // F1 to F4 are 11 to 14 (rxvt); 16 and 22 are gaps in the numbering.
    CHECK(keyNameFromInput("\x1b[1~") == "<home>");
    CHECK(keyNameFromInput("\x1b[7~") == "<home>");
    CHECK(keyNameFromInput("\x1b[2~") == "<insert>");
    CHECK(keyNameFromInput("\x1b[4~") == "<end>");
    CHECK(keyNameFromInput("\x1b[8~") == "<end>");
    CHECK(keyNameFromInput("\x1b[11~") == "<f1>");
    CHECK(keyNameFromInput("\x1b[12~") == "<f2>");
    CHECK(keyNameFromInput("\x1b[13~") == "<f3>");
    CHECK(keyNameFromInput("\x1b[14~") == "<f4>");
    CHECK(keyNameFromInput("\x1b[17~") == "<f6>");
    CHECK(keyNameFromInput("\x1b[18~") == "<f7>");
    CHECK(keyNameFromInput("\x1b[19~") == "<f8>");
    CHECK(keyNameFromInput("\x1b[20~") == "<f9>");
    CHECK(keyNameFromInput("\x1b[21~") == "<f10>");
    CHECK(keyNameFromInput("\x1b[23~") == "<f11>");
    CHECK(keyNameFromInput("\x1b[16~").empty());
    CHECK(keyNameFromInput("\x1b[22~").empty());
    CHECK(keyNameFromInput("\x1b[2;2~") == "S-<insert>");
    CHECK(keyNameFromInput("\x1b[4;6~") == "C-S-<end>");
}

TEST_CASE("non-key input has no name", "[tui][keyNames]") {
    CHECK(keyNameFromInput("").empty());
    CHECK(keyNameFromInput("\x1b[<0;10;5M").empty());
    CHECK(keyNameFromInput("\x1b[99~").empty());
    CHECK(keyNameFromInput("\xff").empty());
    CHECK(keyNameFromInput("\x1b[?25h").empty());    // a private mode, not a key
    CHECK(keyNameFromInput("\x1b[1;5;2A").empty());  // more than two parameters
    CHECK(keyNameFromInput("\x1b[1;5X").empty());    // no key ends in X
    CHECK(keyNameFromInput("\x1bOX").empty());
}

TEST_CASE("oversized and zero CSI parameters are handled", "[tui][keyNames]") {
    CHECK(keyNameFromInput("\x1b[99999999999~").empty());  // no signed overflow
    CHECK(keyNameFromInput("\x1b[1000~").empty());
    CHECK(keyNameFromInput("\x1b[1;0A") == "<up>");  // modifier 0 means none
    CHECK(keyNameFromInput("\x1b[1;5A") == "C-<up>");
}
