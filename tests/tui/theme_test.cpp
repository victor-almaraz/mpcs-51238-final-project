#include <catch2/catch_test_macros.hpp>

#include <proj/tui/theme.h>

#include <set>
#include <string>

using namespace tui;

TEST_CASE("seven themes with unique names, Terminal first", "[tui][theme]") {
    const auto all = themes();
    CHECK(all.size() == 7);
    CHECK(&defaultTheme() == &all[0]);
    CHECK(defaultTheme().name == "terminal");
    std::set<std::string_view> names;
    for (const auto& t : all) names.insert(t.name);
    CHECK(names.size() == all.size());
    CHECK(findTheme("nord") != nullptr);
    CHECK(findTheme("adwaita-dark")->title == "Adwaita Dark");
    CHECK(findTheme("no-such-theme") == nullptr);
}

TEST_CASE("contrast ratio matches WCAG", "[tui][theme]") {
    CHECK(contrastRatio(0x000000, 0xffffff) > 20.9);
    CHECK(contrastRatio(0xffffff, 0xffffff) < 1.01);
    CHECK(contrastRatio(0x777777, 0xffffff) > 4.4);
}

// Every truecolor theme keeps text legible in every role: 4.5:1 for body
// text, 3:1 for secondary text, syntax, status, and button labels, and
// 2.5:1 for syntax inside a selection. High Contrast keeps 7:1.
TEST_CASE("every truecolor theme meets its contrast targets", "[tui][theme]") {
    for (const auto& t : themes()) {
        if (!t.window.rgb) continue;  // Terminal follows the terminal's own palette
        INFO(t.title);
        auto ratio = [](const swatch& a, const swatch& b) { return contrastRatio(*a.rgb, *b.rgb); };
        const double body = t.name == "high-contrast" ? 7.0 : 4.5;
        CHECK(ratio(t.text, t.window) >= body);
        CHECK(ratio(t.secondary, t.window) >= 3.0);
        CHECK(ratio(t.surfaceText, t.surface) >= 4.5);
        CHECK(ratio(t.surfaceSecondary, t.surface) >= 3.0);
        CHECK(ratio(t.surfaceFieldText, t.surfaceField) >= 4.5);
        CHECK(ratio(t.fieldText, t.field) >= 4.5);
        CHECK(ratio(t.selectionText, t.selection) >= 4.5);
        CHECK(ratio(t.onAccent, t.accent) >= 3.0);
        CHECK(ratio(t.onDestructive, t.destructive) >= 3.0);
        CHECK(ratio(t.accentText, t.surface) >= 3.0);
        CHECK(ratio(t.error, t.window) >= 3.0);
        CHECK(ratio(t.info, t.window) >= 3.0);
        CHECK(ratio(t.success, t.window) >= 3.0);
        CHECK(ratio(t.surface, t.window) >= 1.1);  // surfaces stand apart from the window
        for (std::size_t i = 0; i < t.syntax.size(); ++i) {
            INFO("syntax category " << i);
            CHECK(ratio(t.syntax[i].fg, t.window) >= (t.name == "high-contrast" ? 7.0 : 3.0));
            CHECK(ratio(t.syntax[i].fg, t.editorSelection) >= 2.5);
        }
    }
}

TEST_CASE("high contrast draws nothing faint", "[tui][theme]") {
    const theme& hc = *findTheme("high-contrast");
    CHECK_FALSE(hc.faintSecondary);
    for (const auto& s : hc.syntax) CHECK_FALSE(s.faint);
}
