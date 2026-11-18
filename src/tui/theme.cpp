#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <espresso/tui/theme.h>

namespace tui {

using ftxui::Color;

swatch hex(std::uint32_t rgb) {
    return {.color = Color::RGB(static_cast<std::uint8_t>(rgb >> 16u), static_cast<std::uint8_t>(rgb >> 8u),
                                static_cast<std::uint8_t>(rgb)),
            .rgb = rgb};
}

swatch named(Color color) { return {.color = color, .rgb = std::nullopt}; }

double contrastRatio(std::uint32_t a, std::uint32_t b) {
    auto luminance = [](std::uint32_t rgb) {
        auto channel = [](std::uint32_t v) {
            const double c = static_cast<double>(v & 0xFFu) / 255.0;
            return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(rgb >> 16u) + 0.7152 * channel(rgb >> 8u) + 0.0722 * channel(rgb);
    };
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

namespace {

// The color a fraction @p t of the way from @p a to @p b, per channel.
std::uint32_t mix(std::uint32_t a, std::uint32_t b, double t) {
    auto channel = [&](unsigned shift) {
        const double from = (a >> shift) & 0xFFu;
        const double to = (b >> shift) & 0xFFu;
        return static_cast<std::uint32_t>(std::lround(from + (to - from) * t)) << shift;
    };
    return channel(16u) | channel(8u) | channel(0u);
}

// The states of a control drawn as a label over a fill. Each state moves
// the fill a step further (hover one, pressed two) away from the label,
// which raises the label's contrast, or toward the label when the fill is
// already near black or white and the step would not show. The label
// becomes pure black or white, whichever it is nearer, so it stays at
// least as legible as at rest.
controlStates stateSteps(std::uint32_t fill, std::uint32_t label) {
    const bool lightLabel = contrastRatio(label, 0x000000) > contrastRatio(label, 0xffffff);
    const std::uint32_t strong = lightLabel ? 0xffffff : 0x000000;
    const std::uint32_t away = lightLabel ? 0x000000 : 0xffffff;
    constexpr double step = 0.12;
    const bool visible = contrastRatio(mix(fill, away, step), fill) >= 1.15;
    const std::uint32_t target = visible ? away : label;
    return {.hover = {.background = hex(mix(fill, target, step)), .text = hex(strong)},
            .pressed = {.background = hex(mix(fill, target, 2 * step)), .text = hex(strong)}};
}

// Fills in the control states of a truecolor theme from its other roles,
// every one of which has an RGB value.
theme withStates(theme t) {
    auto of = [](const swatch& fill, const swatch& label) {
        return stateSteps(fill.rgb.value(), label.rgb.value());
    };
    t.flatStates = of(t.window, t.text);
    t.surfaceFlatStates = of(t.surface, t.surfaceText);
    t.fieldStates = of(t.field, t.fieldText);
    t.buttonStates = of(t.surfaceField, t.surfaceFieldText);
    t.accentStates = of(t.accent, t.onAccent);
    t.destructiveStates = of(t.destructive, t.onDestructive);
    t.selectionStates = of(t.selection, t.selectionText);
    return t;
}

// Control states in the terminal's palette, which cannot be mixed: each
// state is the next step along the palette's grays, or a light variant
// of the fill's hue.
controlStates stepped(Color hover, Color hoverText, Color pressed, Color pressedText) {
    return {.hover = {.background = named(hover), .text = named(hoverText)},
            .pressed = {.background = named(pressed), .text = named(pressedText)}};
}

syntaxStyle plain(swatch fg) { return {.fg = fg}; }
syntaxStyle strong(swatch fg) { return {.fg = fg, .bold = true}; }
syntaxStyle faint(swatch fg) { return {.fg = fg, .bold = false, .faint = true}; }
syntaxStyle wrong(swatch fg) { return {.fg = fg, .bold = false, .faint = false, .underline = true}; }

// The terminal's own palette: follows the terminal theme, including
// light, dark, and high-contrast terminal settings.
theme terminal() {
    theme t;
    t.name = "terminal";
    t.title = "Terminal";
    t.window = named(Color::Default);
    t.text = named(Color::Default);
    t.secondary = named(Color::Default);
    t.faintSecondary = true;
    t.rule = named(Color::Default);
    t.surface = named(Color::GrayDark);
    t.surfaceText = named(Color::White);
    t.surfaceSecondary = named(Color::GrayLight);
    t.surfaceRule = named(Color::White);
    t.surfaceField = named(Color::GrayLight);
    t.surfaceFieldText = named(Color::Black);
    t.field = named(Color::GrayDark);
    t.fieldText = named(Color::Default);
    t.accent = named(Color::Blue);
    t.onAccent = named(Color::White);
    t.accentText = named(Color::BlueLight);
    t.destructive = named(Color::Red);
    t.onDestructive = named(Color::White);
    t.selection = named(Color::GrayDark);
    t.selectionText = named(Color::Default);
    t.flatStates = stepped(Color::GrayDark, Color::Default, Color::GrayLight, Color::Black);
    t.surfaceFlatStates = stepped(Color::GrayLight, Color::Black, Color::White, Color::Black);
    t.fieldStates = stepped(Color::GrayLight, Color::Black, Color::White, Color::Black);
    t.buttonStates = stepped(Color::White, Color::Black, Color::GrayDark, Color::White);
    t.accentStates = stepped(Color::BlueLight, Color::Black, Color::White, Color::Blue);
    t.destructiveStates = stepped(Color::RedLight, Color::Black, Color::White, Color::Red);
    t.selectionStates = stepped(Color::GrayLight, Color::Black, Color::White, Color::Black);
    t.editorSelection = named(Color::GrayDark);
    t.matchParen = named(Color::Blue);
    t.success = named(Color::Green);
    t.error = named(Color::Red);
    t.info = named(Color::Blue);
    t.syntax = {faint(named(Color::Default)), plain(named(Color::Green)),
                plain(named(Color::Yellow)),  strong(named(Color::Yellow)),
                plain(named(Color::Cyan)),    strong(named(Color::Magenta)),
                plain(named(Color::Magenta)), plain(named(Color::Default)),
                faint(named(Color::Default)), wrong(named(Color::Red))};
    return t;
}

// libadwaita's dark style and the GNOME palette.
theme adwaitaDark() {
    theme t;
    t.name = "adwaita-dark";
    t.title = "Adwaita Dark";
    t.window = hex(0x242424);
    t.text = hex(0xffffff);
    t.secondary = hex(0x9a9996);
    t.rule = hex(0x4a4a4a);
    t.surface = hex(0x3a3a3a);
    t.surfaceText = hex(0xffffff);
    t.surfaceSecondary = hex(0xc0bfbc);
    t.surfaceRule = hex(0x77767b);
    t.surfaceField = hex(0x525252);
    t.surfaceFieldText = hex(0xffffff);
    t.field = hex(0x363636);
    t.fieldText = hex(0xffffff);
    t.accent = hex(0x3584e4);
    t.onAccent = hex(0xffffff);
    t.accentText = hex(0x99c1f1);
    t.destructive = hex(0xc01c28);
    t.onDestructive = hex(0xffffff);
    t.selection = hex(0x3a3a3a);
    t.selectionText = hex(0xffffff);
    t.editorSelection = hex(0x203450);
    t.matchParen = hex(0x1c71d8);
    t.success = hex(0x57e389);
    t.error = hex(0xff7b63);
    t.info = hex(0x99c1f1);
    t.syntax = {plain(hex(0x9a9996)),  plain(hex(0x8ff0a4)), plain(hex(0xffbe6f)),
                strong(hex(0xffbe6f)), plain(hex(0x99c1f1)), strong(hex(0xdc8add)),
                plain(hex(0xdc8add)),  plain(hex(0xffffff)), plain(hex(0x9a9996)),
                wrong(hex(0xff7b63))};
    return t;
}

// libadwaita's light style and the GNOME palette.
theme adwaitaLight() {
    theme t;
    t.name = "adwaita-light";
    t.title = "Adwaita Light";
    t.window = hex(0xfafafa);
    t.text = hex(0x2e2e32);
    t.secondary = hex(0x6a6a70);
    t.rule = hex(0xc0bfbc);
    t.surface = hex(0xdeddda);
    t.surfaceText = hex(0x241f31);
    t.surfaceSecondary = hex(0x5e5c64);
    t.surfaceRule = hex(0x9a9996);
    t.surfaceField = hex(0xffffff);
    t.surfaceFieldText = hex(0x241f31);
    t.field = hex(0xebebed);
    t.fieldText = hex(0x2e2e32);
    t.accent = hex(0x1c71d8);
    t.onAccent = hex(0xffffff);
    t.accentText = hex(0x1a5fb4);
    t.destructive = hex(0xc01c28);
    t.onDestructive = hex(0xffffff);
    t.selection = hex(0xdeddda);
    t.selectionText = hex(0x241f31);
    t.editorSelection = hex(0xd7e7fa);
    t.matchParen = hex(0x99c1f1);
    t.success = hex(0x1d7a46);
    t.error = hex(0xc01c28);
    t.info = hex(0x1a5fb4);
    t.syntax = {plain(hex(0x6a6a70)),  plain(hex(0x1d7a46)), plain(hex(0xb04400)),
                strong(hex(0xb04400)), plain(hex(0x1a5fb4)), strong(hex(0x813d9c)),
                plain(hex(0x813d9c)),  plain(hex(0x2e2e32)), plain(hex(0x77767b)),
                wrong(hex(0xc01c28))};
    return t;
}

// Ethan Schoonover's Solarized, dark and light variants (MIT license).
// Comments use base00 rather than base01/base1, and the light variant
// darkens cyan, yellow, and green slightly, so that every syntax color
// keeps at least 3:1 contrast with the background.
theme solarized(bool dark) {
    constexpr std::uint32_t base03 = 0x002b36, base02 = 0x073642, base01 = 0x586e75,
                            base00 = 0x657b83, base0 = 0x839496, base1 = 0x93a1a1,
                            base2 = 0xeee8d5, base3 = 0xfdf6e3;
    constexpr std::uint32_t orange = 0xcb4b16, red = 0xdc322f, magenta = 0xd33682,
                            violet = 0x6c71c4, blue = 0x268bd2;
    const std::uint32_t yellow = dark ? 0xb58900 : 0xa87f00;
    const std::uint32_t cyan = dark ? 0x2aa198 : 0x26948b;
    const std::uint32_t green = dark ? 0x859900 : 0x7b8e00;
    theme t;
    t.name = dark ? "solarized-dark" : "solarized-light";
    t.title = dark ? "Solarized Dark" : "Solarized Light";
    const std::uint32_t bg = dark ? base03 : base3;
    const std::uint32_t bgHigh = dark ? base02 : base2;
    const std::uint32_t body = dark ? base1 : base01;
    const std::uint32_t emphasis = dark ? base2 : base02;
    t.window = hex(bg);
    t.text = hex(body);
    t.secondary = hex(base00);
    t.rule = hex(dark ? base01 : base1);
    t.surface = hex(bgHigh);
    t.surfaceText = hex(emphasis);
    t.surfaceSecondary = hex(dark ? base1 : base01);
    t.surfaceRule = hex(dark ? base0 : base00);
    t.surfaceField = hex(bg);
    t.surfaceFieldText = hex(emphasis);
    t.field = hex(bgHigh);
    t.fieldText = hex(emphasis);
    t.accent = hex(blue);
    t.onAccent = hex(base3);
    t.accentText = hex(dark ? blue : 0x1f74b0);
    t.destructive = hex(red);
    t.onDestructive = hex(base3);
    t.selection = hex(bgHigh);
    t.selectionText = hex(emphasis);
    t.editorSelection = hex(bgHigh);  // Solarized's own selection color
    t.matchParen = hex(dark ? base01 : 0xd3cbb1);
    t.success = hex(green);
    t.error = hex(red);
    t.info = hex(blue);
    t.syntax = {plain(hex(base00)),   plain(hex(cyan)),   plain(hex(magenta)),
                strong(hex(orange)),  plain(hex(yellow)), strong(hex(green)),
                plain(hex(violet)),   plain(hex(body)),   plain(hex(base00)),
                wrong(hex(red))};
    return t;
}

// Arctic Ice Studio's Nord (MIT license).
theme nord() {
    theme t;
    t.name = "nord";
    t.title = "Nord";
    t.window = hex(0x2e3440);
    t.text = hex(0xd8dee9);
    t.secondary = hex(0x8a95aa);
    t.rule = hex(0x4c566a);
    t.surface = hex(0x434c5e);
    t.surfaceText = hex(0xeceff4);
    t.surfaceSecondary = hex(0xc2c9d6);
    t.surfaceRule = hex(0x81a1c1);
    t.surfaceField = hex(0x2e3440);
    t.surfaceFieldText = hex(0xeceff4);
    t.field = hex(0x3b4252);
    t.fieldText = hex(0xe5e9f0);
    t.accent = hex(0x5e81ac);
    t.onAccent = hex(0xeceff4);
    t.accentText = hex(0x88c0d0);
    t.destructive = hex(0xbf616a);
    t.onDestructive = hex(0x2e3440);
    t.selection = hex(0x3b4252);
    t.selectionText = hex(0xeceff4);
    t.editorSelection = hex(0x3b4252);
    t.matchParen = hex(0x5e81ac);
    t.success = hex(0xa3be8c);
    t.error = hex(0xd57780);  // aurora red, lightened to stay legible on selections
    t.info = hex(0x88c0d0);
    t.syntax = {plain(hex(0x8a95aa)),  plain(hex(0xa3be8c)), plain(hex(0xb48ead)),
                strong(hex(0xb48ead)), plain(hex(0x8fbcbb)), strong(hex(0x81a1c1)),
                plain(hex(0x88c0d0)),  plain(hex(0xd8dee9)), plain(hex(0x8a95aa)),
                wrong(hex(0xd57780))};
    return t;
}

// Maximum contrast, no faint text, and colors kept apart by luminance as
// well as hue (HIG: respect high contrast).
theme highContrast() {
    theme t;
    t.name = "high-contrast";
    t.title = "High Contrast";
    t.window = hex(0x000000);
    t.text = hex(0xffffff);
    t.secondary = hex(0xd0d0d0);
    t.rule = hex(0xffffff);
    t.surface = hex(0x262626);
    t.surfaceText = hex(0xffffff);
    t.surfaceSecondary = hex(0xe0e0e0);
    t.surfaceRule = hex(0xffffff);
    t.surfaceField = hex(0xffffff);
    t.surfaceFieldText = hex(0x000000);
    t.field = hex(0xffffff);
    t.fieldText = hex(0x000000);
    t.accent = hex(0xffff00);
    t.onAccent = hex(0x000000);
    t.accentText = hex(0xffff00);
    t.destructive = hex(0xff5f5f);
    t.onDestructive = hex(0x000000);
    t.selection = hex(0xffffff);
    t.selectionText = hex(0x000000);
    t.editorSelection = hex(0x00405f);
    t.matchParen = hex(0x8700af);
    t.success = hex(0x00ff7f);
    t.error = hex(0xff5f5f);
    t.info = hex(0x5fd7ff);
    t.syntax = {plain(hex(0xd0d0d0)), plain(hex(0x00ff7f)),
                plain(hex(0xffd700)), strong(hex(0xffd700)),
                plain(hex(0x5fd7ff)), strong(hex(0xff87ff)),
                plain(hex(0xff87ff)), plain(hex(0xffffff)),
                plain(hex(0xd0d0d0)), {.fg = hex(0xff5f5f), .bold = true, .faint = false, .underline = true}};
    return t;
}

const std::vector<theme>& all() {
    static const std::vector<theme> list{terminal(),
                                         withStates(adwaitaDark()),
                                         withStates(adwaitaLight()),
                                         withStates(solarized(true)),
                                         withStates(solarized(false)),
                                         withStates(nord()),
                                         withStates(highContrast())};
    return list;
}

} // namespace

std::span<const theme> themes() { return all(); }

optionalRef<theme> findTheme(std::string_view name) {
    for (const auto& t : all()) {
        if (t.name == name) return std::cref(t);
    }
    return std::nullopt;
}

const theme& defaultTheme() { return all().front(); }

} // namespace tui
