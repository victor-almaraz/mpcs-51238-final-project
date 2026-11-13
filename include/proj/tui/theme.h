/// @file
/// Editor themes. Every theme assigns a color to the same set of roles,
/// so views ask for a role ("secondary text", "surface", "accent") and
/// never for a literal color. Truecolor themes fall back to 256 or 16
/// colors on terminals without truecolor support (`tui.md` §6.6).

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <ftxui/screen/color.hpp>

#include <proj/tui/highlighter.h>

namespace tui {

/// A theme color: the FTXUI color and, for RGB colors, its 0xRRGGBB
/// value (used to check contrast).
struct swatch {
    ftxui::Color color = ftxui::Color::Default;
    std::optional<std::uint32_t> rgb;

    operator ftxui::Color() const { return color; }  // NOLINT: implicit by design
};

/// An RGB swatch.
swatch hex(std::uint32_t rgb);

/// A swatch from the terminal's own palette.
swatch named(ftxui::Color color);

/// How one highlight category is drawn.
struct syntaxStyle {
    swatch fg;
    bool bold = false;
    bool faint = false;
    bool underline = false;
};

/// The color roles of the editor.
struct theme {
    std::string_view name;   ///< Settings value, such as "adwaita-dark"
    std::string_view title;  ///< Display name, such as "Adwaita Dark"
    bool dark = true;

    // The window: background, text, secondary text, and rules.
    swatch window, text, secondary, rule;
    bool faintSecondary = false;  ///< Draw secondary text faint instead of in a color

    // Raised surfaces: popovers, dialogs, overlays, the open prompt.
    swatch surface, surfaceText, surfaceSecondary, surfaceRule;
    swatch surfaceField, surfaceFieldText;  ///< Buttons and key labels on a surface

    // Controls on the window.
    swatch field, fieldText;              ///< Entry-like controls in the header bar
    swatch accent, onAccent, accentText;  ///< Suggested action; accent-colored text
    swatch destructive, onDestructive;
    swatch selection, selectionText;      ///< Current file and tab

    // The editor.
    swatch editorSelection, matchParen;

    // Status.
    swatch success, error, info;

    /// Indexed by highlightCategory (`tui.md` §6.2).
    std::array<syntaxStyle, 10> syntax;

    const syntaxStyle& styleOf(highlightCategory c) const {
        return syntax[static_cast<std::size_t>(c)];
    }
};

/// The built-in themes, the default first.
std::span<const theme> themes();

/// The theme named @p name, or nothing.
const theme* findTheme(std::string_view name);

/// The default theme, which uses the terminal's own palette.
const theme& defaultTheme();

/// WCAG 2 contrast ratio between two RGB colors (1 to 21).
double contrastRatio(std::uint32_t a, std::uint32_t b);

} // namespace tui
