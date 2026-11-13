#include <catch2/catch_test_macros.hpp>

#include <proj/tui/app.h>
#include <proj/tui/fileStore.h>

#include "support/tempDir.h"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using namespace tui;

namespace {

struct project : support::tempDir {
    const fs::path root = path();
    project() {
        write("main.lisp", "(module main)\n");
        write("voices.lisp", "(module voices)\n");
    }
    std::string read(const std::string& name) const {
        return *fileStore::read(root / name).text;
    }
    appOptions options(keyScheme scheme = keyScheme::emacs) const {
        const fs::path settings = root / ".settings.lisp";
        std::ofstream(settings) << "(settings :keys :" << toString(scheme) << ")";
        // No server: requests go to an unused port and give up quickly.
        return {root, {}, settings, false, linkOptions{{1, 5, 0}, "test", false}};
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

struct frame {
    ftxui::Screen screen;
    explicit frame(app& a, int width = 100, int height = 30)
        : screen(ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                       ftxui::Dimension::Fixed(height))) {
        ftxui::Render(screen, a.render());
    }

    std::string at(int x, int y) const { return screen.CellAt(x, y).character; }

    bool contains(const std::string& needle) const {
        for (int y = 0; y < screen.dimy(); ++y) {
            std::string row;
            for (int x = 0; x < screen.dimx(); ++x) row += at(x, y).empty() ? " " : at(x, y);
            if (row.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    // Position of the first occurrence of `needle` at or right of column `minX`.
    std::pair<int, int> find(const std::string& needle, int minX = 0) const {
        for (int y = 0; y < screen.dimy(); ++y) {
            std::string row;
            std::vector<int> xs;
            for (int x = 0; x < screen.dimx(); ++x) {
                xs.push_back(static_cast<int>(row.size()));
                const std::string& c = screen.CellAt(x, y).character;
                row += c.empty() ? " " : c;
            }
            for (std::size_t at = row.find(needle); at != std::string::npos; at = row.find(needle, at + 1)) {
                const auto x = static_cast<int>(std::find(xs.begin(), xs.end(), static_cast<int>(at)) - xs.begin());
                if (x >= minX) return {x, y};
            }
        }
        FAIL("not on screen: " << needle);
        return {-1, -1};
    }
};

void click(app& a, std::pair<int, int> at, ftxui::Mouse::Button button = ftxui::Mouse::Left) {
    ftxui::Mouse m;
    m.button = button;
    m.motion = ftxui::Mouse::Pressed;
    m.x = at.first;
    m.y = at.second;
    a.handleMouse(m);
    m.motion = ftxui::Mouse::Released;
    a.handleMouse(m);
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

TEST_CASE("vim scheme: insert, escape, and :w", "[tui][app]") {
    project p;
    app a(p.options(keyScheme::vim));
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
    app a(p.options(keyScheme::cua));
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

TEST_CASE("mouse: file list, tabs, and editor clicks (tui.md 4.1)", "[tui][app]") {
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
    CHECK(a.keys().scheme() == keyScheme::cua);

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
    app a(p.options(keyScheme::cua));
    a.runCommand("showShortcuts");
    const frame f(a);
    CHECK(f.contains("Cua key bindings"));
    CHECK(f.contains("Ctrl+S"));
    CHECK(f.contains("Open File…"));
}

TEST_CASE("the save alert: Enter saves, and the dialog is modal", "[tui][app][hig]") {
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

TEST_CASE("play reports read errors that jump to their span (tui.md 9.3, 9.4)", "[tui][app]") {
    project p;
    p.write("broken.lisp", "(define x\n  (svf :in 2x))\n");
    app a(p.options());
    click(a, frame(a).find("▶ Play"));
    a.link().waitIdle();
    a.collectReplies();
    bool sawError = false;
    for (const auto& e : a.console().entries()) {
        if (e.kind == consoleKind::error && e.location) {
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

TEST_CASE("dialogs, overlays, menus, and the prompt are raised surfaces", "[tui][app][hig]") {
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
          t.styleOf(highlightCategory::specialForm).fg.color);

    // Surfaces take the theme's surface color.
    a.runCommand("mainMenu");
    const frame menu(a);
    const auto item = menu.find("Switch Buffer…");
    CHECK(menu.screen.CellAt(item.first, item.second).background_color == t.surface.color);
    a.handleKey("ESC");

    CHECK(loadSettings(p.root / ".settings.lisp").values.theme == "adwaita-dark");
}

TEST_CASE("an unknown theme falls back and says so", "[tui][app][theme]") {
    project p;
    std::ofstream(p.root / ".settings.lisp") << "(settings :theme :sepia)";
    app a({p.root, {}, p.root / ".settings.lisp", false, linkOptions{{1, 5, 0}, "test", false}});
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
        for (auto overlay : {"showPreferences", "showShortcuts", "showAbout"}) {
            a.runCommand(overlay);
            CHECK(frame(a).contains("×"));
        }
        a.runCommand("showPreferences");
        a.handleKey("RET");  // the Theme row: next theme
    }
    CHECK(a.currentTheme().name == "terminal");  // seven steps return to the start
}
