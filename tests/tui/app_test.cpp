#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <espresso/tui/app.h>
#include <espresso/tui/keymap.h>
#include <espresso/tui/theme.h>
#include <espresso/workspace/consoleLog.h>
#include <espresso/workspace/fileStore.h>
#include <espresso/workspace/highlighter.h>
#include <espresso/workspace/interpreterLink.h>
#include <espresso/workspace/settings.h>

#include "support/frame.h"
#include "support/tempDir.h"
#include "support/unwrap.h"

namespace fs = std::filesystem;
using namespace tui;
using support::frame;

namespace {

struct project : support::tempDir {
    const fs::path root = path();
    project() {
        write("main.lisp", "(module main)\n");
        write("voices.lisp", "(module voices)\n");
    }
    std::string read(const std::string& name) const {
        return support::unwrap(workspace::fileStore::read(root / name).text);
    }
    appOptions options(workspace::keyScheme scheme = workspace::keyScheme::emacs) const {
        return options("(settings :keys :" + std::string(toString(scheme)) + ")");
    }
    // Options whose settings file holds @p settingsText.
    appOptions options(std::string_view settingsText) const {
        const fs::path settings = root / ".settings.lisp";
        std::ofstream(settings) << settingsText;
        // No server: requests go to an unused port and give up quickly.
        return {.projectRoot = root,
                .files = {},
                .settingsFile = settings,
                .persistSettings = false,
                .link = workspace::linkOptions{.server = {.serverPort = 1, .timeoutMs = 5, .retries = 0},
                                               .clientVersion = "test",
                                               .connectOnStart = false}};
    }
};

void type(app& a, std::string_view text) {
    for (char c : text) {
        a.handleKey(c == ' ' ? std::string("SPC") : c == '\n' ? std::string("RET") : std::string(1, c));
    }
}

void keys(app& a, std::initializer_list<std::string_view> ks) {
    for (auto k : ks) a.handleKey(k);
}

// One mouse event at @p at.
void mouse(app& a, std::pair<int, int> at, ftxui::Mouse::Button button, ftxui::Mouse::Motion motion) {
    ftxui::Mouse m;
    m.button = button;
    m.motion = motion;
    m.x = at.first;
    m.y = at.second;
    a.handleMouse(m);
}

void click(app& a, std::pair<int, int> at, ftxui::Mouse::Button button = ftxui::Mouse::Left) {
    mouse(a, at, button, ftxui::Mouse::Pressed);
    mouse(a, at, button, ftxui::Mouse::Released);
}

} // namespace

TEST_CASE("app opens main.lisp and saves edits (emacs)", "[tui][app]") {
    project p;
    app a(p.options());
    REQUIRE(a.current().path() == p.root / "main.lisp");
    keys(a, {"M->"});
    type(a, "(play main)");
    CHECK(a.current().dirty());
    keys(a, {"C-x", "C-s"});
    CHECK_FALSE(a.current().dirty());
    CHECK(p.read("main.lisp") == "(module main)\n(play main)");
}

TEST_CASE("closing a dirty buffer asks first (tui.md 7.4)", "[tui][app]") {
    project p;
    app a(p.options());
    a.openFile("voices.lisp");
    REQUIRE(a.bufferCount() == 2);
    type(a, "x");
    keys(a, {"C-x", "k"});
    REQUIRE(a.dialogOpen());
    a.handleKey("c");
    CHECK(a.bufferCount() == 2);
    keys(a, {"C-x", "k", "d"});
    CHECK(a.bufferCount() == 1);
    CHECK(p.read("voices.lisp") == "(module voices)\n");
}

TEST_CASE("quit asks about each dirty buffer (tui.md 7.5)", "[tui][app]") {
    project p;
    app a(p.options());
    a.openFile("voices.lisp");
    type(a, "v");
    a.handleKey("C-x");
    a.handleKey("<left>");  // previous buffer: main.lisp
    type(a, "m");
    keys(a, {"C-x", "C-c"});
    REQUIRE(a.dialogOpen());
    a.handleKey("s");
    REQUIRE(a.dialogOpen());
    CHECK_FALSE(a.quitRequested());
    a.handleKey("d");
    CHECK(a.quitRequested());
    CHECK(p.read("main.lisp") == "m(module main)\n");
    CHECK(p.read("voices.lisp") == "(module voices)\n");
}

TEST_CASE("vim scheme: insert then escape then :w", "[tui][app]") {
    project p;
    app a(p.options(workspace::keyScheme::vim));
    CHECK(a.keys().mode() == editMode::normal);
    type(a, "x");  // deletes "("
    type(a, "i[");
    a.handleKey("ESC");
    CHECK(a.keys().mode() == editMode::normal);
    type(a, ":w\n");
    CHECK(p.read("main.lisp") == "[module main)\n");
    type(a, "dd");
    CHECK(a.current().text() == "");
    type(a, "u");
    CHECK(a.current().text() == "[module main)\n");
}

TEST_CASE("cua scheme: shift selection and clipboard", "[tui][app]") {
    project p;
    app a(p.options(workspace::keyScheme::cua));
    keys(a, {"<right>", "S-<right>", "S-<right>", "S-<right>", "S-<right>", "S-<right>", "S-<right>"});
    CHECK(a.current().selectedText() == "module");
    keys(a, {"C-x"});
    CHECK(a.current().text() == "( main)\n");
    keys(a, {"<end>", "C-v"});
    CHECK(a.current().text() == "( main)module\n");
    keys(a, {"C-z", "C-z"});
    CHECK(a.current().text() == "(module main)\n");
}

TEST_CASE("incremental search moves the cursor", "[tui][app]") {
    project p;
    p.write("main.lisp", "alpha beta gamma beta");
    app a(p.options());
    keys(a, {"C-s", "b", "e"});
    CHECK(a.current().cursor().column == 8);
    keys(a, {"C-s"});
    CHECK(a.current().cursor().column == 19);
    keys(a, {"RET"});
    CHECK_FALSE(a.prompt().active());
    keys(a, {"C-s", "z", "C-g"});
    CHECK(a.current().cursor().column == 19);  // cancel returns to the origin
}

TEST_CASE("open file prompt completes project files", "[tui][app]") {
    project p;
    app a(p.options());
    keys(a, {"C-x", "C-f", "v", "TAB"});
    CHECK(a.prompt().text() == "voices.lisp");
    keys(a, {"RET"});
    CHECK(a.current().path() == p.root / "voices.lisp");
}

TEST_CASE("mouse: file list and tab and editor clicks (tui.md 4.1)", "[tui][app]") {
    project p;
    app a(p.options());
    click(a, frame(a).find("voices.lisp"));
    CHECK(a.current().path() == p.root / "voices.lisp");
    CHECK(a.bufferCount() == 2);

    click(a, frame(a).find(" main.lisp ", 20));  // the tab, right of the file list
    CHECK(a.current().path() == p.root / "main.lisp");

    const auto at = frame(a).find("(module main)", 20);
    click(a, {at.first + 3, at.second});
    CHECK(a.current().cursor() == position{0, 3});
}

TEST_CASE("a linked file shows its own name and saves through the link", "[tui][app]") {
    support::tempDir examples;
    examples.write("phasing/loops.lisp", "(module loops)\n");
    project p;
    fs::create_symlink(examples / "phasing/loops.lisp", p.root / "loops.lisp");
    app a(p.options());
    const frame f(a);
    CHECK_FALSE(f.contains("phasing"));
    click(a, f.find(" loops.lisp"));  // the file list
    CHECK(a.current().path() == p.root / "loops.lisp");
    CHECK(frame(a).find(" loops.lisp ", 20).first > 20);  // the tab
    a.current().insert(";");
    a.runCommand("save");
    CHECK(fs::is_symlink(p.root / "loops.lisp"));
    CHECK(support::unwrap(workspace::fileStore::read(examples / "phasing/loops.lisp").text) ==
          ";(module loops)\n");
}

TEST_CASE("closing a tab left of the current one keeps the current buffer", "[tui][app]") {
    project p;
    app a(p.options());
    a.openFile("voices.lisp");
    a.openFile("third.lisp");                      // tabs: main, voices, third
    click(a, frame(a).find(" voices.lisp ", 20));  // the middle tab is current
    REQUIRE(a.current().path() == p.root / "voices.lisp");
    const auto tab = frame(a).find(" main.lisp ", 20);
    click(a, {tab.first + static_cast<int>(std::string_view(" main.lisp ").size()), tab.second});  // its ×
    REQUIRE(a.bufferCount() == 2);
    CHECK(a.current().path() == p.root / "voices.lisp");
}

TEST_CASE("a click outside the prompt cancels it (Save As cannot name another buffer)", "[tui][app]") {
    project p;
    app a(p.options());
    a.openFile("voices.lisp");
    click(a, frame(a).find(" main.lisp ", 20));
    keys(a, {"C-x", "C-w"});  // Save As, for main.lisp
    REQUIRE(a.prompt().active());
    click(a, frame(a).find(" voices.lisp ", 20));
    CHECK_FALSE(a.prompt().active());
    CHECK(a.current().path() == p.root / "voices.lisp");
    type(a, "renamed.lisp\n");  // now edits voices.lisp instead of answering a prompt
    CHECK_FALSE(fs::exists(p.root / "renamed.lisp"));
    a.openFile("main.lisp");
    CHECK(a.current().path() == p.root / "main.lisp");
}

TEST_CASE("cutting the last line without a selection leaves no empty line", "[tui][app]") {
    project p;
    app a(p.options(workspace::keyScheme::cua));
    keys(a, {"C-<end>"});
    type(a, "\n(play main)");
    REQUIRE(a.current().text() == "(module main)\n\n(play main)");
    a.runCommand("cut");
    CHECK(a.current().text() == "(module main)\n");
    a.runCommand("cut");  // the same rule as deleteLine on every line
    CHECK(a.current().text() == "(module main)");
}

TEST_CASE("mouse: the main menu runs commands", "[tui][app][hig]") {
    project p;
    app a(p.options());
    REQUIRE(a.currentSettings().fileList);
    click(a, frame(a).find("≡"));
    REQUIRE(a.menuOpen());
    const frame f(a);
    CHECK(f.contains("✓ Show File List"));
    CHECK(f.contains("Open File…"));    // an ellipsis marks items that ask for more input
    CHECK_FALSE(f.contains("Quit"));    // quitting is the header bar's close button
    click(a, f.find("Show File List"));
    CHECK_FALSE(a.menuOpen());
    CHECK_FALSE(a.currentSettings().fileList);
}

TEST_CASE("right-click opens the context menu", "[tui][app][hig]") {
    project p;
    app a(p.options());
    const auto at = frame(a).find("(module main)", 20);
    click(a, {at.first + 2, at.second}, ftxui::Mouse::Right);
    REQUIRE(a.menuOpen());
    click(a, frame(a).find("Select All"));
    CHECK(a.current().selectedText() == "(module main)\n");
}

// Clickable targets come from the last render; a key can close the popover
// or modal that drew them before the next render, and a press on such a
// stale target ran a closed menu's row (reading the empty menu) or changed
// a closed overlay's setting.
TEST_CASE("a press on a menu that a key closed does nothing", "[tui][app][hig]") {
    project p;
    app a(p.options());
    const auto at = frame(a).find("(module main)", 20);
    click(a, {at.first + 2, at.second}, ftxui::Mouse::Right);
    REQUIRE(a.menuOpen());
    const auto selectAll = frame(a).find("Select All");
    a.handleKey("ESC");
    REQUIRE_FALSE(a.menuOpen());
    click(a, selectAll);  // no render in between: the menu's targets remain
    CHECK(a.current().selectedText().empty());
    CHECK_FALSE(a.menuOpen());
}

TEST_CASE("a press on a menu that another menu replaced does nothing", "[tui][app][hig]") {
    project p;
    app a(p.options());
    const auto at = frame(a).find("(module main)", 20);
    click(a, {at.first + 2, at.second}, ftxui::Mouse::Right);  // the context menu
    REQUIRE(a.menuOpen());
    const auto selectAll = frame(a).find("Select All");  // its seventh row
    a.handleKey("ESC");
    a.handleKey("<f10>");  // the main menu, whose seventh row is Find…; no render in between
    REQUIRE(a.menuOpen());
    click(a, selectAll);
    CHECK_FALSE(a.prompt().active());  // Find… did not run
    CHECK(a.current().selectedText().empty());
}

TEST_CASE("a press on an overlay that a key closed does nothing", "[tui][app][hig]") {
    project p;
    app a(p.options());
    a.runCommand("showPreferences");
    const auto keyBindings = frame(a).find("Key Bindings");
    a.handleKey("ESC");
    REQUIRE(a.overlayName().empty());
    click(a, keyBindings);
    CHECK(a.keys().scheme() == workspace::keyScheme::emacs);
}

TEST_CASE("menus work from the keyboard (F10)", "[tui][app][hig]") {
    project p;
    app a(p.options());
    a.handleKey("<f10>");
    REQUIRE(a.menuOpen());
    a.handleKey("<up>");  // wraps to the last item, About
    a.handleKey("<up>");  // Keyboard Shortcuts
    a.handleKey("RET");
    CHECK_FALSE(a.menuOpen());
    CHECK(a.overlayName() == "shortcuts");
    a.handleKey("ESC");
    CHECK(a.overlayName().empty());

    a.handleKey("<f10>");
    a.handleKey("ESC");
    CHECK_FALSE(a.menuOpen());
    CHECK(a.current().text() == "(module main)\n");  // keys went to the menu, not the buffer
}

TEST_CASE("preferences apply immediately", "[tui][app][hig]") {
    project p;
    app a(p.options());
    a.runCommand("showPreferences");
    REQUIRE(a.overlayName() == "preferences");
    const frame f(a);
    CHECK(f.contains("Key Bindings"));
    CHECK(f.contains("50 ms"));
    CHECK_FALSE(f.contains("Apply"));
    click(a, f.find("Key Bindings"));
    CHECK(a.keys().scheme() == workspace::keyScheme::cua);

    a.handleKey("<down>");   // Fade
    a.handleKey("<right>");  // +10 ms
    CHECK(a.currentSettings().fade > 0.059);
    a.handleKey("<down>");
    a.handleKey("<down>");
    a.handleKey("<down>");   // Show File List
    a.handleKey("SPC");
    CHECK_FALSE(a.currentSettings().fileList);

    // The dialog is modal: clicks outside it do nothing.
    const std::size_t before = a.current().cursorOffset();
    click(a, {2, 28});
    CHECK(a.overlayName() == "preferences");
    CHECK(a.current().cursorOffset() == before);
}

TEST_CASE("the shortcuts dialog shows the scheme's notation", "[tui][app][hig]") {
    project p;
    app a(p.options(workspace::keyScheme::cua));
    a.runCommand("showShortcuts");
    const frame f(a);
    CHECK(f.contains("Cua key bindings"));
    CHECK(f.contains("Ctrl+S"));
    CHECK(f.contains("Open File…"));
}

TEST_CASE("the save alert: Enter saves and the dialog is modal", "[tui][app][hig]") {
    project p;
    app a(p.options());
    type(a, "x");
    keys(a, {"C-x", "k"});
    REQUIRE(a.dialogOpen());
    const frame f(a);
    CHECK(f.contains("Save Changes?"));
    CHECK(f.contains("Cancel"));
    CHECK(f.contains("Discard"));
    click(a, f.find("(module main)", 20));  // outside the dialog: ignored
    CHECK(a.dialogOpen());
    a.handleKey("RET");
    CHECK_FALSE(a.dialogOpen());
    CHECK(p.read("main.lisp") == "x(module main)\n");
}

TEST_CASE("panels line up", "[tui][app][hig]") {
    project p;
    app a(p.options());
    const frame warmup(a);  // first layout
    const frame f(a);
    const auto heading = f.find(" " + p.root.filename().string());
    const auto tab = f.find(" main.lisp ", 20);
    CHECK(heading.second == tab.second);  // sidebar heading and tab bar share a row
    const int divider = 24;               // the default file list width
    CHECK(f.at(divider, tab.second) == "│");
    CHECK(f.at(divider, tab.second + 1) == "┼");  // one continuous rule under both
    CHECK(f.at(divider, tab.second - 1) == "┬");
    CHECK(f.contains("Console"));
}

TEST_CASE("narrow terminals collapse the sidebar", "[tui][app][hig]") {
    project p;
    app a(p.options());
    const frame warmup(a, 60, 24);  // first layout
    const frame f(a, 60, 24);
    CHECK_FALSE(f.contains("voices.lisp"));
    CHECK(f.contains("▶ Play"));
    CHECK(f.contains("≡"));
}

TEST_CASE("play reports read errors that jump to their span (tui.md 9.3 and 9.4)", "[tui][app]") {
    project p;
    p.write("broken.lisp", "(define x\n  (svf :in 2x))\n");
    app a(p.options());
    click(a, frame(a).find("▶ Play"));
    a.link().waitIdle();
    a.collectReplies();
    bool sawError = false;
    for (const auto& e : a.console().entries()) {
        if (e.kind == workspace::consoleKind::error && e.location) {
            sawError = true;
            CHECK(e.location->path == fs::path("broken.lisp"));
            CHECK(e.location->line == 2);
            CHECK(e.location->column == 12);
        }
    }
    REQUIRE(sawError);
    click(a, frame(a).find("broken.lisp:2:12"));
    CHECK(a.current().path() == p.root / "broken.lisp");
    CHECK(a.current().cursor() == position{1, 11});
}

TEST_CASE("a program nested past the depth limit reports an error instead of crashing the editor",
          "[tui][app]") {
    // The interpreter worker is a espresso::jthread; its stack must hold
    // interpreter::maxEvalDepth levels (512 KiB on macOS did not).
    project p;
    p.write("main.lisp", "(define (f n) (if (= n 0) 0 (+ 1 (f (- n 1)))))\n(f 5000)\n");
    app a(p.options());
    a.runCommand("play");
    a.link().waitIdle();
    a.collectReplies();
    bool sawDepthError = false;
    for (const auto& e : a.console().entries()) {
        if (e.kind == workspace::consoleKind::error && e.text.find("nested too deeply") != std::string::npos)
            sawDepthError = true;
    }
    CHECK(sawDepthError);
}

TEST_CASE("play reports compile errors at their call (interpreter.md 5.4)", "[tui][app]") {
    project p;
    p.write("main.lisp", "(defpiece main ()\n  (sine-osc :freq 2s :out (pipe :audio)))\n");
    app a(p.options());
    click(a, frame(a).find("▶ Play"));
    a.link().waitIdle();
    a.collectReplies();
    bool sawError = false;
    for (const auto& e : a.console().entries()) {
        if (e.kind == workspace::consoleKind::error && e.location) {
            sawError = true;
            CHECK(e.text ==
                  "main.lisp:2:19: parameter :freq expects a frequency, got a time");  // at the value
            CHECK(e.location->line == 2);
            CHECK(e.location->column == 19);
        }
    }
    CHECK(sawError);
}

TEST_CASE("play builds the entry piece that project.lisp names", "[tui][app]") {
    project p;
    p.write("project.lisp", "(project :name \"drift\" :entry intro)\n");
    p.write("main.lisp", "(defpiece intro ()\n  (sine-osc :out (pipe :audio)))\n");
    app a(p.options());
    click(a, frame(a).find("▶ Play"));
    a.link().waitIdle();
    a.collectReplies();
    const auto& entries = a.console().entries();
    CHECK(std::ranges::any_of(entries,
                              [](const auto& e) { return e.text.starts_with("play: compiled main.lisp"); }));
}

TEST_CASE("play reports an invalid project.lisp at its span", "[tui][app]") {
    project p;
    p.write("project.lisp", "(project :name \"drift\"\n  :page-port 80)\n");
    p.write("main.lisp", "(defpiece main ()\n  (sine-osc :out (pipe :audio)))\n");
    app a(p.options());
    click(a, frame(a).find("▶ Play"));
    a.link().waitIdle();
    a.collectReplies();
    bool sawError = false;
    for (const auto& e : a.console().entries()) {
        CHECK_FALSE(e.text.starts_with("play: compiled"));
        if (e.kind == workspace::consoleKind::error && e.location) {
            sawError = true;
            CHECK(e.text == "project.lisp:2:14: :page-port takes a port from 1024 to 65535");
            CHECK(e.location->path == "project.lisp");
            CHECK(e.location->line == 2);
        }
    }
    CHECK(sawError);
}

TEST_CASE("play shows compile warnings at their call", "[tui][app]") {
    project p;
    p.write("main.lisp", "(defpiece main ()\n  (sine-osc :freq 30khz :out (pipe :audio)))\n");
    app a(p.options());
    click(a, frame(a).find("▶ Play"));
    a.link().waitIdle();
    a.collectReplies();
    bool found = false;
    for (const auto& e : a.console().entries()) {
        if (e.text.starts_with("main.lisp:2:3: warning: parameter freq is 30000 Hz") && e.location) found = true;
    }
    CHECK(found);
}

TEST_CASE("evalLastForm sends the form before the cursor", "[tui][app]") {
    project p;
    p.write("main.lisp", "(a)\n(b 1)");
    app a(p.options());
    keys(a, {"M->", "C-x", "C-e"});
    a.link().waitIdle();
    a.collectReplies();
    bool found = false;
    for (const auto& e : a.console().entries()) {
        if (e.text.find("eval main.lisp:2:1: read (b 1)") != std::string::npos) found = true;
    }
    CHECK(found);
}

TEST_CASE("the header bar keeps its end controls", "[tui][app][hig]") {
    project p;
    app a(p.options());
    const frame warmup(a);
    const frame f(a);
    std::string row;
    for (int x = 0; x < 100; ++x) row += f.at(x, 0).empty() ? "_" : f.at(x, 0);
    INFO(row);
    CHECK(row.find("≡") != std::string::npos);
    CHECK(row.find("×") != std::string::npos);
}

TEST_CASE("dialogs and overlays and menus and the prompt are raised surfaces", "[tui][app][hig]") {
    project p;
    app a(p.options());
    const frame warmup(a);
    auto onSurface = [&](const std::string& text) {
        const frame f(a);
        const auto at = f.find(text);
        return f.screen.CellAt(at.first, at.second).background_color == ftxui::Color::GrayDark;
    };
    auto windowIsPlain = [&] {
        const frame f(a);
        const auto at = f.find("(module main)", 20);
        return !(f.screen.CellAt(at.first + 3, at.second).background_color ==
                 ftxui::Color::GrayDark);
    };
    REQUIRE(windowIsPlain());

    a.runCommand("mainMenu");
    CHECK(onSurface("Switch Buffer…"));
    a.handleKey("ESC");

    a.runCommand("showPreferences");
    CHECK(onSurface("Playback"));
    a.handleKey("ESC");

    a.runCommand("showShortcuts");
    CHECK(onSurface("Editing"));
    a.handleKey("ESC");

    a.runCommand("showAbout");
    CHECK(onSurface("A workstation"));
    a.handleKey("ESC");

    type(a, "x");
    keys(a, {"C-x", "k"});
    CHECK(onSurface("Save Changes?"));
    a.handleKey("c");

    keys(a, {"C-x", "C-f"});
    CHECK(onSurface("Open file:"));
    a.handleKey("C-g");
    CHECK(windowIsPlain());
}

TEST_CASE("themes apply everywhere and persist (Preferences)", "[tui][app][theme]") {
    project p;
    auto options = p.options();
    options.persistSettings = true;
    app a(options);
    CHECK(a.currentTheme().name == "terminal");

    a.runCommand("showPreferences");
    click(a, frame(a).find("Theme"));  // the first row cycles to the next theme
    CHECK(a.currentTheme().name == "adwaita-dark");
    CHECK(frame(a).contains("Adwaita Dark ▾"));
    a.handleKey("ESC");

    // The window and the editor take the theme's colors.
    const theme& t = a.currentTheme();
    const frame f(a);
    const auto code = f.find("(module main)", 20);
    CHECK(f.screen.CellAt(code.first + 3, code.second).background_color == t.window.color);
    CHECK(f.screen.CellAt(code.first + 1, code.second).foreground_color ==
          t.styleOf(workspace::highlightCategory::specialForm).fg.color);

    // Surfaces take the theme's surface color.
    a.runCommand("mainMenu");
    const frame menu(a);
    const auto item = menu.find("Switch Buffer…");
    CHECK(menu.screen.CellAt(item.first, item.second).background_color == t.surface.color);
    a.handleKey("ESC");

    CHECK(workspace::loadSettings(p.root / ".settings.lisp").values.theme == "adwaita-dark");
}

TEST_CASE("an unknown theme falls back and says so", "[tui][app][theme]") {
    project p;
    std::ofstream(p.root / ".settings.lisp") << "(settings :theme :sepia)";
    app a({.projectRoot = p.root,
           .files = {},
           .settingsFile = p.root / ".settings.lisp",
           .persistSettings = false,
           .link = workspace::linkOptions{.server = {.serverPort = 1, .timeoutMs = 5, .retries = 0},
                                          .clientVersion = "test",
                                          .connectOnStart = false}});
    CHECK(a.currentTheme().name == "terminal");
    bool reported = false;
    for (const auto& e : a.console().entries()) {
        if (e.text.find("unknown theme :sepia") != std::string::npos) reported = true;
    }
    CHECK(reported);
}

TEST_CASE("every theme renders every view", "[tui][app][theme]") {
    project p;
    auto options = p.options();
    options.persistSettings = true;
    app a(options);
    a.runCommand("showPreferences");
    for (std::size_t i = 0; i < themes().size(); ++i) {
        INFO(a.currentTheme().title);
        for (const auto* overlay : {"showPreferences", "showShortcuts", "showAbout"}) {
            a.runCommand(overlay);
            CHECK(frame(a).contains("×"));
        }
        a.runCommand("showPreferences");
        a.handleKey("RET");  // the Theme row: next theme
    }
    CHECK(a.currentTheme().name == "terminal");  // seven steps return to the start
}

namespace {

// Replaces the current buffer's text with @p text and puts the cursor at
// the start.
void fill(app& a, const std::string& text) {
    a.runCommand("selectAll");
    a.current().deleteSelection();
    a.current().insert(text);
    a.current().setCursor({.line = 0, .column = 0});
}

position at(std::size_t line, std::size_t column) { return {.line = line, .column = column}; }

}  // namespace

TEST_CASE("movement commands move the cursor by character, word, line, page, and buffer", "[tui][app]") {
    project p;
    app a(p.options());
    std::string lines = "  first line here\nsecond line\n";
    for (int i = 0; i < 60; ++i) lines += "line " + std::to_string(i) + "\n";
    fill(a, lines);
    (void)frame(a);  // lays out the editor, so a page has a height
    a.runCommand("forwardWord");
    CHECK(a.current().cursor().column == 7);
    a.runCommand("backwardWord");
    CHECK(a.current().cursor().column == 2);
    a.runCommand("lineEnd");
    CHECK(a.current().cursor().column == 17);
    a.runCommand("lineStart");
    CHECK(a.current().cursor().column == 0);
    a.runCommand("firstNonBlank");
    CHECK(a.current().cursor().column == 2);
    a.runCommand("nextLine");
    a.runCommand("previousLine");
    CHECK(a.current().cursor().line == 0);
    a.runCommand("pageDown");
    const std::size_t page = a.current().cursor().line;
    CHECK(page > 5);
    CHECK(a.current().topLine() > 0);
    a.runCommand("pageUp");
    CHECK(a.current().cursor().line == 0);
    CHECK(a.current().topLine() == 0);
    a.runCommand("bufferEnd");
    CHECK(a.current().cursor().line == a.current().lineCount() - 1);
    a.runCommand("bufferStart");
    CHECK(a.current().cursor() == at(0, 0));
}

TEST_CASE("editing commands kill, yank, copy, and paste lines, and redo", "[tui][app]") {
    project p;
    app a(p.options());
    fill(a, "one\ntwo\nthree\n");
    a.runCommand("killLine");
    a.runCommand("killLine");  // consecutive kills append: "one" and its line break
    CHECK(a.current().text() == "two\nthree\n");
    a.runCommand("paste");
    CHECK(a.current().text() == "one\ntwo\nthree\n");
    a.runCommand("yankLine");
    CHECK(a.message() == "Yanked line");
    a.runCommand("copy");  // no selection: the line
    CHECK(a.message() == "Copied");
    a.runCommand("setMark");
    CHECK(a.message() == "Mark set");
    a.runCommand("cancel");
    CHECK(a.message() == "Quit");
    a.current().setCursor(at(0, 0));
    a.runCommand("newline");
    a.runCommand("indent");  // two spaces
    CHECK(a.current().text().starts_with("\n  one"));
    a.runCommand("deleteBackward");
    a.runCommand("deleteForward");
    CHECK(a.current().text().starts_with("\n ne"));
    a.runCommand("undo");
    a.runCommand("redo");
    CHECK(a.current().text().starts_with("\n ne"));
    a.runCommand("redo");
    CHECK(a.message() == "No further redo information");
}

TEST_CASE("buffer commands cycle and switch between open buffers", "[tui][app]") {
    project p;
    app a(p.options());
    REQUIRE(a.openFile("voices.lisp"));
    REQUIRE(a.bufferCount() == 2);
    const auto voices = a.current().path();
    a.runCommand("nextBuffer");
    CHECK(a.current().path() != voices);
    a.runCommand("previousBuffer");
    CHECK(a.current().path() == voices);
    a.runCommand("switchBuffer");
    CHECK(a.prompt().active());
    a.runCommand("cancel");
    a.runCommand("quitWithoutSaving");
    CHECK(a.quitRequested());
}

TEST_CASE("pane commands clear the console and open the menus", "[tui][app]") {
    project p;
    app a(p.options());
    (void)frame(a);
    a.runCommand("play");  // writes to the console: no server
    a.runCommand("clearConsole");
    CHECK(a.console().entries().empty());
    a.runCommand("mainMenu");
    CHECK(a.menuOpen());
    a.runCommand("cancel");
    CHECK_FALSE(a.menuOpen());
    a.runCommand("contextMenu");
    CHECK(a.menuOpen());
}

TEST_CASE("vim mode commands enter insert mode where vim does", "[tui][app]") {
    project p;
    app a(p.options(workspace::keyScheme::vim));
    fill(a, "  abc\n");
    a.runCommand("appendMode");
    CHECK(a.keys().mode() == editMode::insert);
    CHECK(a.current().cursor() == at(0, 1));
    a.runCommand("normalMode");
    CHECK(a.keys().mode() == editMode::normal);
    CHECK(a.current().cursor() == at(0, 0));  // leaving insert mode steps back
    a.runCommand("appendLineEnd");
    CHECK(a.current().cursor() == at(0, 5));
    a.runCommand("normalMode");
    a.runCommand("insertLineStart");
    CHECK(a.current().cursor() == at(0, 2));
    a.runCommand("normalMode");
    a.runCommand("openLineBelow");
    CHECK(a.current().text() == "  abc\n\n");
    CHECK(a.current().cursor() == at(1, 0));
    a.runCommand("normalMode");
    a.runCommand("openLineAbove");
    CHECK(a.current().text() == "  abc\n\n\n");
    CHECK(a.current().cursor() == at(1, 0));
    a.runCommand("normalMode");
    CHECK(frame(a).contains("Normal"));  // the status bar names the mode
    a.runCommand("visualMode");
    CHECK(a.keys().mode() == editMode::visual);
    CHECK(frame(a).contains("Visual"));
    CHECK(a.current().hasSelection() == false);  // an empty selection until the cursor moves
}

namespace {

// A wheel step at @p at: up scrolls back.
void wheel(app& a, std::pair<int, int> at, bool up) {
    mouse(a, at, up ? ftxui::Mouse::WheelUp : ftxui::Mouse::WheelDown, ftxui::Mouse::Pressed);
}

// A left-button drag from @p from to @p to.
void drag(app& a, std::pair<int, int> from, std::pair<int, int> to) {
    mouse(a, from, ftxui::Mouse::Left, ftxui::Mouse::Pressed);
    mouse(a, to, ftxui::Mouse::Left, ftxui::Mouse::Moved);
    mouse(a, to, ftxui::Mouse::Left, ftxui::Mouse::Released);
}

}  // namespace

TEST_CASE("mouse: the header bar's fade, quantize, menu, and close controls", "[tui][app]") {
    project p;
    app a(p.options());
    click(a, frame(a).find("50 ms"));
    CHECK(a.prompt().active());  // click to type a fade
    a.runCommand("cancel");
    wheel(a, frame(a).find("50 ms"), true);
    CHECK(std::abs(a.currentSettings().fade - 0.05) > 0.005);  // scroll to adjust it
    click(a, frame(a).find("None"));
    CHECK(a.menuOpen());  // the quantize grids
    a.runCommand("cancel");
    click(a, frame(a).find("≡"));
    CHECK(a.menuOpen());
    a.runCommand("cancel");
    click(a, frame(a).find("×", 90));
    CHECK(a.quitRequested());  // nothing unsaved
}

TEST_CASE("mouse: the editor scrolls with the wheel and a drag moves the cursor", "[tui][app]") {
    project p;
    app a(p.options());
    std::string lines;
    for (int i = 0; i < 80; ++i) lines += "line " + std::to_string(i) + "\n";
    fill(a, lines);
    wheel(a, frame(a).find("line 5"), false);
    CHECK(a.current().topLine() == 3);
    wheel(a, frame(a).find("line 5"), true);
    CHECK(a.current().topLine() == 0);
    const frame f(a);
    drag(a, f.find("line 2"), f.find("line 6"));
    CHECK(a.current().cursor().line == 6);
}

TEST_CASE("a message longer than the window wraps in the message line", "[tui][app]") {
    project p;
    const std::string name = "a-file-whose-name-alone-is-longer-than-half-of-the-window-width.lisp";
    std::ofstream(p.root / name) << "\xff\xfe not UTF-8";
    app a(p.options());
    CHECK_FALSE(a.openFile(name));
    const frame f(a);
    // The console shows the error too; the message line is the bottom of the window.
    CHECK(f.find("is not valid UTF-8", 0, f.screen.dimy() - 2).second >= f.screen.dimy() - 2);
}

TEST_CASE("mouse: the wheel does not scroll the editor behind a modal", "[tui][app][hig]") {
    project p;
    app a(p.options());
    std::string lines;
    for (int i = 0; i < 80; ++i) lines += "line " + std::to_string(i) + "\n";
    fill(a, lines);
    const auto at = frame(a).find("line 5");
    a.runCommand("showAbout");
    (void)frame(a);  // lays out the overlay
    wheel(a, at, false);
    CHECK(a.current().topLine() == 0);
}

TEST_CASE("mouse: dragging the console heading and the file list edge resizes them", "[tui][app]") {
    project p;
    app a(p.options());
    const int height = a.currentSettings().consoleHeight;
    const auto heading = frame(a).find("Console");
    drag(a, heading, {heading.first, heading.second - 4});
    CHECK(a.currentSettings().consoleHeight > height);
    const int width = a.currentSettings().fileListWidth;
    const auto edge = frame(a).find("│1 ");  // the file list's right edge, beside the editor
    drag(a, {edge.first, edge.second + 3}, {edge.first + 6, edge.second + 3});
    CHECK(a.currentSettings().fileListWidth > width);
}

TEST_CASE("mouse: overlays close and scroll, and preferences cycle with the wheel", "[tui][app]") {
    project p;
    app a(p.options());
    a.runCommand("showAbout");
    click(a, frame(a).find("Close"));
    CHECK(a.overlayName().empty());
    a.runCommand("showShortcuts");
    const auto title = frame(a, 100, 20).find("key bindings");
    const auto rows = [&a] {
        const frame f(a, 100, 20);
        std::string all;
        for (int y = 0; y < f.screen.dimy(); ++y) {
            for (int x = 0; x < f.screen.dimx(); ++x) all += f.at(x, y);
        }
        return all;
    };
    const std::string before = rows();
    wheel(a, {title.first, title.second + 4}, false);
    CHECK(rows() != before);  // the bindings scrolled
    a.runCommand("cancel");
    a.runCommand("showPreferences");
    const auto quant = a.currentSettings().quant;
    wheel(a, frame(a).find("Quantize", 0, 2), false);  // the overlay's row, below the header bar
    CHECK(a.currentSettings().quant != quant);
}

TEST_CASE("mouse: a click on a prompt's completion chooses it", "[tui][app]") {
    project p;
    app a(p.options());
    keys(a, {"C-x", "C-f", "v"});  // the prompt lists the files that match
    const frame f(a);
    click(a, f.find("voices.lisp", 0, f.screen.dimy() - 1));  // on the prompt's row, not in the file list
    CHECK(a.current().path() == p.root / "voices.lisp");
}

// ---------------------------------------------------------------------
// Control states (tui.md 4.3)

namespace {

void pointer(app& a, std::pair<int, int> at, ftxui::Mouse::Motion motion) {
    mouse(a, at, motion == ftxui::Mouse::Moved ? ftxui::Mouse::None : ftxui::Mouse::Left, motion);
}

void hover(app& a, std::pair<int, int> at) { pointer(a, at, ftxui::Mouse::Moved); }

// An app in a truecolor theme, whose state colors differ from each other.
struct themed {
    project p;
    app a{p.options("(settings :theme :adwaita-dark)")};
    const theme& t() const { return a.currentTheme(); }
};

ftxui::Color background(app& a, std::pair<int, int> at) {
    return frame(a).screen.CellAt(at.first, at.second).background_color;
}

ftxui::Color foreground(app& a, std::pair<int, int> at) {
    return frame(a).screen.CellAt(at.first, at.second).foreground_color;
}

}  // namespace

TEST_CASE("controls light up under the pointer and return to rest when it leaves", "[tui][app][hig]") {
    themed s;
    app& a = s.a;
    REQUIRE(s.t().name == "adwaita-dark");
    const auto play = frame(a).find("▶ Play");
    const auto menu = frame(a).find("≡");
    const auto fade = frame(a).find("50 ms");
    CHECK(background(a, play) == s.t().accent.color);

    hover(a, play);
    CHECK(background(a, play) == s.t().accentStates.hover.background.color);
    CHECK(foreground(a, play) == s.t().accentStates.hover.text.color);
    hover(a, menu);
    CHECK(background(a, play) == s.t().accent.color);  // only the control under the pointer
    CHECK(background(a, menu) == s.t().flatStates.hover.background.color);
    hover(a, fade);
    CHECK(background(a, fade) == s.t().fieldStates.hover.background.color);
    hover(a, frame(a).find("(module main)", 20));  // the editor shows no state
    CHECK(background(a, menu) == s.t().window.color);
    CHECK(background(a, fade) == s.t().field.color);
}

TEST_CASE("a control is pressed while the button is held on it", "[tui][app][hig]") {
    themed s;
    app& a = s.a;
    const auto plus = frame(a).find(" + ");
    pointer(a, plus, ftxui::Mouse::Pressed);  // opens the Open File prompt
    CHECK(a.prompt().active());
    CHECK(background(a, plus) == s.t().flatStates.pressed.background.color);
    hover(a, frame(a).find("(module main)", 20));
    CHECK(background(a, plus) == s.t().window.color);  // the pointer left it
    hover(a, plus);
    CHECK(background(a, plus) == s.t().flatStates.pressed.background.color);  // and came back
    pointer(a, plus, ftxui::Mouse::Released);
    CHECK(background(a, plus) == s.t().flatStates.hover.background.color);
}

TEST_CASE("a divider lights up under the pointer and while it is dragged", "[tui][app][hig]") {
    themed s;
    app& a = s.a;
    const auto heading = frame(a).find("Console");
    hover(a, heading);
    CHECK(foreground(a, {heading.first - 2, heading.second}) == s.t().accentText.color);
    pointer(a, heading, ftxui::Mouse::Pressed);
    pointer(a, {heading.first, heading.second - 3}, ftxui::Mouse::Moved);
    const auto moved = frame(a).find("Console");
    CHECK(moved.second == heading.second - 3);
    CHECK(foreground(a, {moved.first - 2, moved.second}) == s.t().accentText.color);
    pointer(a, moved, ftxui::Mouse::Released);
    hover(a, frame(a).find("(module main)", 20));
    CHECK_FALSE(foreground(a, {moved.first - 2, moved.second}) == s.t().accentText.color);
}

TEST_CASE("Clear on the console heading clears the console", "[tui][app]") {
    project p;
    app a(p.options());
    a.runCommand("play");  // no server: the console reports it
    a.link().waitIdle();
    a.collectReplies();
    REQUIRE(a.console().size() > 0);
    const int height = a.currentSettings().consoleHeight;
    click(a, frame(a).find("Clear"));
    CHECK(a.console().size() == 0);
    CHECK(a.currentSettings().consoleHeight == height);  // the press reached Clear, not the heading
}

TEST_CASE("long console lines wrap, and the console scrolls by rows", "[tui][app]") {
    project p;
    const std::string name = "a-file-whose-long-name-runs-past-the-edge-of-the-console.lisp";
    p.write(name, "caf\xe9");
    app a(p.options());
    REQUIRE_FALSE(a.openFile(name));  // the console reports the encoding error
    const std::string message = a.console().entries().back().text;
    REQUIRE(message.size() > 100);  // wider than the frame

    const frame f(a);  // the default 100 x 30
    const auto [x, y] = f.find(message.substr(0, 12));
    CHECK(f.at(x - 2, y) == "✕");
    auto squeeze = [](std::string s) {
        std::erase(s, ' ');
        return s;
    };
    std::string shown;
    int row = y;
    for (; row < f.screen.dimy() && squeeze(shown).size() < squeeze(message).size(); ++row) {
        if (row > y) CHECK(squeeze(f.at(x - 2, row)).empty());  // wrapped rows line up after the mark
        for (int column = x; column < f.screen.dimx(); ++column) shown += f.at(column, row);
    }
    CHECK(row - y > 1);
    CHECK(squeeze(shown) == squeeze(message));

    // More wrapped entries than the pane holds: the wheel moves one row.
    for (int i = 0; i < 4; ++i) {
        p.write(std::to_string(i) + ".lisp", "\xff");
        a.openFile(std::to_string(i) + ".lisp");
    }
    auto text = [](const frame& at, int line) {
        std::string s;
        for (int column = 25; column < at.screen.dimx(); ++column) s += at.at(column, line);
        return s;
    };
    const frame before(a);
    const int bottom = before.find("Console").second + a.currentSettings().consoleHeight;
    CHECK(text(before, bottom).find("UTF-8") != std::string::npos);  // the newest row
    wheel(a, {60, bottom}, true);
    CHECK(text(frame(a), bottom) == text(before, bottom - 1));
    wheel(a, {60, bottom}, false);
    CHECK(text(frame(a), bottom) == text(before, bottom));
}

TEST_CASE("Stop cannot be used until a plan plays", "[tui][app][hig]") {
    themed s;
    app& a = s.a;
    const auto stop = frame(a).find("■ Stop");
    CHECK(foreground(a, stop) == s.t().secondary.color);
    hover(a, stop);
    CHECK(background(a, stop) == s.t().window.color);  // no hover state
    CHECK(frame(a).contains("Stop after the fade"));   // its tooltip still says what it does
    const std::size_t messages = a.console().size();
    click(a, stop);
    CHECK(a.console().size() == messages);  // the press does nothing
}

TEST_CASE("a menu's focus follows the pointer and skips items that cannot be used", "[tui][app][hig]") {
    themed s;
    app& a = s.a;
    click(a, frame(a).find("(module main)", 20), ftxui::Mouse::Right);
    REQUIRE(a.menuOpen());
    // Nothing to undo, redo, paste, or find again: focus starts on Cut.
    const frame f(a);
    const auto cut = f.find("Cut");
    CHECK(f.screen.CellAt(cut.first, cut.second).background_color == s.t().accent.color);
    const auto undo = f.find("Undo");
    CHECK(f.screen.CellAt(undo.first, undo.second).foreground_color == s.t().surfaceSecondary.color);
    a.handleKey("<up>");  // past Redo and Undo, around to Evaluate Form
    const frame up(a);
    const auto eval = up.find("Evaluate Form");
    CHECK(up.screen.CellAt(eval.first, eval.second).background_color == s.t().accent.color);

    const auto selectAll = up.find("Select All");
    hover(a, selectAll);
    CHECK(background(a, selectAll) == s.t().accent.color);  // focus moved to it
    CHECK_FALSE(background(a, eval) == s.t().accent.color);
    hover(a, undo);
    CHECK(background(a, selectAll) == s.t().accent.color);  // Undo takes no focus
    click(a, undo);
    CHECK(a.menuOpen());  // and does nothing
}

TEST_CASE("controls behind a modal show no state", "[tui][app][hig]") {
    themed s;
    app& a = s.a;
    const auto play = frame(a).find("▶ Play");
    a.runCommand("showAbout");
    hover(a, play);
    CHECK_FALSE(background(a, play) == s.t().accentStates.hover.background.color);
    CHECK_FALSE(frame(a).contains("Compile and play"));  // nor its tooltip
    const auto close = frame(a).find("Close", 0, 3);
    hover(a, close);
    CHECK(background(a, close) == s.t().accentStates.hover.background.color);
}
