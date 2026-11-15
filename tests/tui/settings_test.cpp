#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <filesystem>
#include <optional>
#include <string>

#include <proj/tui/settings.h>

#include "support/tempDir.h"

namespace fs = std::filesystem;
using namespace tui;

TEST_CASE("settings path follows XDG (tui.md 8.8)", "[tui][settings]") {
    CHECK(settingsPath("/xdg", "/home/u") == fs::path("/xdg/proj/settings.lisp"));
    CHECK(settingsPath("", "/home/u") == fs::path("/home/u/.config/proj/settings.lisp"));
    CHECK(settingsPath(std::nullopt, "/home/u") == fs::path("/home/u/.config/proj/settings.lisp"));
    CHECK(settingsPath("relative/dir", "/home/u") ==
          fs::path("/home/u/.config/proj/settings.lisp"));
    // Without an absolute base there is no settings file, rather than one
    // relative to the working directory.
    CHECK_FALSE(settingsPath(std::nullopt, std::nullopt));
    CHECK_FALSE(settingsPath("", ""));
    CHECK_FALSE(settingsPath("relative/dir", "home/u"));
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

TEST_CASE("durations format and parse one way everywhere", "[tui][settings]") {
    CHECK(formatMilliseconds(0.05) == "50ms");
    CHECK(formatMilliseconds(0.0125, true) == "12.5 ms");
    CHECK(parseDuration("250ms") == 0.25);
    CHECK(parseDuration("2s") == 2.0);
    CHECK(parseDuration("50", 1e-3) == 0.05);  // plain numbers in the caller's unit
    CHECK_FALSE(parseDuration("2hz"));
    CHECK_FALSE(parseDuration("-5ms"));
    CHECK_FALSE(parseDuration("1 2"));
    CHECK_FALSE(parseDuration(""));
}

TEST_CASE("settings read the theme name", "[tui][settings]") {
    const auto r = parseSettings("(settings :theme :nord)", "s");
    CHECK(r.errors.empty());
    CHECK(r.values.theme == "nord");
    CHECK(parseSettings("(settings :theme \"nord\")", "s").errors.size() == 1);
    CHECK(settings{}.theme == "terminal");
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
    s.theme = "solarized-light";
    const auto r = parseSettings(formatSettings(s), "s");
    CHECK(r.errors.empty());
    CHECK(r.values == s);

    const support::tempDir dir;
    CHECK(loadSettings(dir / "none.lisp").values == settings{});
    CHECK(saveSettings(dir / "proj" / "settings.lisp", s).empty());
    CHECK(loadSettings(dir / "proj" / "settings.lisp").values == s);
}
