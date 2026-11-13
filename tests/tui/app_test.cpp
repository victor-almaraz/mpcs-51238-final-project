#include <catch2/catch_test_macros.hpp>

#include <proj/tui/app.h>
#include <proj/tui/fileStore.h>

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

struct project {
    fs::path root;
    project() {
        std::string tmpl = (fs::temp_directory_path() / "proj-app-XXXXXX").string();
        root = ::mkdtemp(tmpl.data());
        write("main.lisp", "(module main)\n");
        write("voices.lisp", "(module voices)\n");
    }
    ~project() { fs::remove_all(root); }
    void write(const std::string& name, const std::string& text) const {
        std::ofstream(root / name, std::ios::binary) << text;
    }
    std::string read(const std::string& name) const {
        return *fileStore::read(root / name).text;
    }
    appOptions options(keyScheme scheme = keyScheme::emacs) const {
        const fs::path settings = root / ".settings.lisp";
        std::ofstream(settings) << "(settings :keys :" << toString(scheme) << ")";
        return {root, {}, settings, false};
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
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(100),
                                                 ftxui::Dimension::Fixed(30));
    explicit frame(app& a) { ftxui::Render(screen, a.render()); }

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

void click(app& a, std::pair<int, int> at) {
    ftxui::Mouse m;
    m.button = ftxui::Mouse::Left;
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

TEST_CASE("mouse: menus run commands", "[tui][app]") {
    project p;
    app a(p.options());
    REQUIRE(a.currentSettings().fileList);
    click(a, frame(a).find("View"));
    click(a, frame(a).find("Hide File List"));
    CHECK_FALSE(a.currentSettings().fileList);

    click(a, frame(a).find("Edit"));
    click(a, frame(a).find("Select All"));
    CHECK(a.current().selectedText() == "(module main)\n");
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
