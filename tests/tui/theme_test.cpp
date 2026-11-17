#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <functional>
#include <set>
#include <string_view>

#include <espresso/tui/theme.h>

#include "support/unwrap.h"

using namespace tui;

TEST_CASE("seven themes with unique names and Terminal first", "[tui][theme]") {
    const auto all = themes();
    CHECK(all.size() == 7);
    CHECK(&defaultTheme() == all.data());
    CHECK(defaultTheme().name == "terminal");
    std::set<std::string_view> names;
    for (const auto& t : all) names.insert(t.name);
    CHECK(names.size() == all.size());
    CHECK(findTheme("nord"));
    CHECK(support::unwrap(findTheme("adwaita-dark")).get().title == "Adwaita Dark");
    CHECK_FALSE(findTheme("no-such-theme"));
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

// A control's hover and pressed fills differ visibly from its resting
// fill and from each other, and its label stays as legible as at rest.
TEST_CASE("every truecolor theme's control states are visible and legible (tui.md 4.3)", "[tui][theme]") {
    for (const auto& t : themes()) {
        if (!t.window.rgb) continue;
        auto ratio = [](const swatch& a, const swatch& b) { return contrastRatio(*a.rgb, *b.rgb); };
        struct kind {
            std::string_view name;
            const swatch& rest;
            const controlStates& states;
            double label;
        };
        const double body = t.name == "high-contrast" ? 7.0 : 4.5;
        for (const kind& k :
             {kind{.name = "flat", .rest = t.window, .states = t.flatStates, .label = body},
              kind{.name = "surface flat", .rest = t.surface, .states = t.surfaceFlatStates, .label = 4.5},
              kind{.name = "field", .rest = t.field, .states = t.fieldStates, .label = 4.5},
              kind{.name = "button", .rest = t.surfaceField, .states = t.buttonStates, .label = 4.5},
              kind{.name = "accent", .rest = t.accent, .states = t.accentStates, .label = 3.0},
              kind{.name = "destructive", .rest = t.destructive, .states = t.destructiveStates, .label = 3.0},
              kind{.name = "selection", .rest = t.selection, .states = t.selectionStates, .label = 4.5}}) {
            INFO(t.title << ": " << k.name);
            const stateColors& hover = k.states.hover;
            const stateColors& pressed = k.states.pressed;
            CHECK(ratio(hover.background, k.rest) >= 1.1);
            CHECK(ratio(pressed.background, k.rest) >= 1.1);
            CHECK(ratio(pressed.background, hover.background) >= 1.1);
            CHECK(ratio(hover.text, hover.background) >= k.label);
            CHECK(ratio(pressed.text, pressed.background) >= k.label);
        }
        CHECK(ratio(t.handle, t.window) >= 3.0);
    }
}

TEST_CASE("the terminal theme gives every control state a color", "[tui][theme]") {
    const theme& term = defaultTheme();
    for (const controlStates& s :
         {std::cref(term.flatStates), std::cref(term.surfaceFlatStates), std::cref(term.fieldStates),
          std::cref(term.buttonStates), std::cref(term.accentStates), std::cref(term.destructiveStates),
          std::cref(term.selectionStates)}) {
        CHECK(s.hover.background.color != s.pressed.background.color);
    }
}

TEST_CASE("high contrast draws nothing faint", "[tui][theme]") {
    const theme& hc = support::unwrap(findTheme("high-contrast")).get();
    CHECK_FALSE(hc.faintSecondary);
    for (const auto& s : hc.syntax) CHECK_FALSE(s.faint);
}
