#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <proj/tui/settings.h>

#include <filesystem>
#include <string>

#include <unistd.h>

namespace fs = std::filesystem;
using namespace tui;

TEST_CASE("settings path follows XDG (tui.md 8.8)", "[tui][settings]") {
    CHECK(settingsPath("/xdg", "/home/u") == fs::path("/xdg/proj/settings.lisp"));
    CHECK(settingsPath("", "/home/u") == fs::path("/home/u/.config/proj/settings.lisp"));
    CHECK(settingsPath(nullptr, "/home/u") == fs::path("/home/u/.config/proj/settings.lisp"));
    CHECK(settingsPath("relative/dir", "/home/u") ==
          fs::path("/home/u/.config/proj/settings.lisp"));
}

TEST_CASE("settings parse the documented form", "[tui][settings]") {
    const auto r = parseSettings(R"((settings
  :keys        :vim
  :file-list   #f
  :console     #t
  :fade        250ms
  :quant       :bar
  :rebuild-on-save #t))",
                                 "settings.lisp");
    CHECK(r.errors.empty());
    CHECK(r.values.keys == keyScheme::vim);
    CHECK_FALSE(r.values.fileList);
    CHECK(r.values.console);
    CHECK_THAT(r.values.fade, Catch::Matchers::WithinAbs(0.25, 1e-12));
    CHECK(r.values.quant == quantization::bar);
    CHECK(r.values.rebuildOnSave);
}

TEST_CASE("unknown settings name the setting and file (tui.md 8.9)", "[tui][settings]") {
    const auto r = parseSettings("(settings :colour :red :keys :cua)", "/cfg/settings.lisp");
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].find(":colour") != std::string::npos);
    CHECK(r.errors[0].find("/cfg/settings.lisp") != std::string::npos);
    CHECK(r.values.keys == keyScheme::cua);
}

TEST_CASE("settings report bad values and syntax", "[tui][settings]") {
    CHECK(parseSettings("(settings :fade 2hz)", "s").errors.size() == 1);
    CHECK(parseSettings("(settings :keys :ed)", "s").errors.size() == 1);
    CHECK(parseSettings("(settings :console 1)", "s").errors.size() == 1);
    CHECK(parseSettings("(settings :keys", "s").errors.size() == 1);
    CHECK(parseSettings("(other)", "s").errors.size() == 1);
}

TEST_CASE("settings round-trip and missing files use defaults", "[tui][settings]") {
    settings s;
    s.keys = keyScheme::nano;
    s.fade = 0.0125;
    s.fileListWidth = 30;
    s.console = false;
    const auto r = parseSettings(formatSettings(s), "s");
    CHECK(r.errors.empty());
    CHECK(r.values == s);

    std::string tmpl = (fs::temp_directory_path() / "proj-settings-XXXXXX").string();
    const fs::path dir = ::mkdtemp(tmpl.data());
    CHECK(loadSettings(dir / "none.lisp").values == settings{});
    CHECK(saveSettings(dir / "proj" / "settings.lisp", s).empty());
    CHECK(loadSettings(dir / "proj" / "settings.lisp").values == s);
    fs::remove_all(dir);
}
