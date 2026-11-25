/// @file
/// Editor themes. Every theme assigns a color to the same set of roles,
/// so views ask for a role ("secondary text", "surface", "accent") and
/// never for a literal color. Truecolor themes fall back to 256 or 16
/// colors on terminals without truecolor support (`tui.md` §6.6).

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

#include <ftxui/screen/color.hpp>

#include <espresso/workspace/highlighter.h>

namespace tui {

/// An optional reference to a constant @p T (metadata::optionalRef, which
/// `tui` does not include).
template <typename T>
using optionalRef = std::optional<std::reference_wrapper<const T>>;

/// A theme color: the FTXUI color and, for RGB colors, its 0xRRGGBB
/// value (used to check contrast). Views pass `color` to FTXUI
/// decorators.
struct swatch {
    ftxui::Color color = ftxui::Color::Default;  ///< The color to draw with
    std::optional<std::uint32_t> rgb;            ///< 0xRRGGBB, or nothing for palette colors
};

/// An RGB swatch.
swatch hex(std::uint32_t rgb);

/// A swatch from the terminal's own palette.
swatch named(ftxui::Color color);

/// How one highlight category is drawn.
struct syntaxStyle {
    swatch fg;               ///< Text color
    bool bold = false;       ///< Draw bold
    bool faint = false;      ///< Draw faint (used where the theme has no secondary color)
    bool underline = false;  ///< Draw underlined
};

/// The colors of a control in one interactive state.
struct stateColors {
    swatch background;  ///< Fill across the control
    swatch text;        ///< Text on the fill
};

/// The colors of one kind of control under the pointer and while the
/// pointer presses it (`tui.md` §4.3).
struct controlStates {
    stateColors hover;    ///< Under the pointer
    stateColors pressed;  ///< Pressed, with the pointer still over it
};

/// The color roles of the editor. Views ask for a role, never for a
/// literal color.
struct theme {
    std::string_view name;   ///< Settings value, such as "adwaita-dark"
    std::string_view title;  ///< Display name, such as "Adwaita Dark"

    /// @name The window
    /// @{

    swatch window;                ///< Window background
    swatch text;                  ///< Body text
    swatch secondary;             ///< Secondary text: hints, line numbers, inactive tabs
    swatch rule;                  ///< Separators and pane dividers
    bool faintSecondary = false;  ///< Draw secondary text faint instead of in a color
    /// @}

    /// @name Raised surfaces: popovers, dialogs, overlays, the open prompt
    /// @{

    swatch surface;           ///< Surface background
    swatch surfaceText;       ///< Text on a surface
    swatch surfaceSecondary;  ///< Secondary text on a surface
    swatch surfaceRule;       ///< Borders and separators on a surface
    swatch surfaceField;      ///< Buttons and key labels on a surface
    swatch surfaceFieldText;  ///< Text on surfaceField
    /// @}

    /// @name Controls on the window
    /// @{

    swatch field;          ///< Entry-like controls in the header bar
    swatch fieldText;      ///< Text on field
    swatch accent;         ///< Suggested action and focused rows
    swatch onAccent;       ///< Text on accent
    swatch accentText;     ///< Accent-colored text, such as a switch that is on
    swatch destructive;    ///< Destructive action, such as Discard
    swatch onDestructive;  ///< Text on destructive
    swatch selection;      ///< Current file and current tab
    swatch selectionText;  ///< Text on selection
    /// @}

    /// @name Control states
    /// Hover and pressed colors by kind of control. A control that
    /// cannot be used draws its text in the secondary color of the
    /// window or surface instead, and has no states.
    /// @{

    controlStates flatStates;         ///< Text-only controls on the window: buttons, tabs, list rows
    controlStates surfaceFlatStates;  ///< Text-only controls on a surface
    controlStates fieldStates;        ///< Controls drawn on field
    controlStates buttonStates;       ///< Buttons drawn on surfaceField
    controlStates accentStates;       ///< Controls drawn on accent, and the focused row
    controlStates destructiveStates;  ///< Controls drawn on destructive
    controlStates selectionStates;    ///< The current tab and file
    /// @}

    /// @name The editor
    /// @{

    swatch editorSelection;  ///< Background of selected text
    swatch matchParen;       ///< Background of the delimiter matching the cursor's
    /// @}

    /// @name Status
    /// @{

    swatch success;  ///< Connected state
    swatch error;    ///< Errors in the console
    swatch info;     ///< Server messages in the console
    /// @}

    /// Indexed by highlightCategory (`tui.md` §6.2).
    std::array<syntaxStyle, workspace::highlightCategoryCount> syntax;

    /// The style of category @p c.
    const syntaxStyle& styleOf(workspace::highlightCategory c) const {
        return syntax[static_cast<std::size_t>(c)];
    }
};

/// The built-in themes, the default first.
std::span<const theme> themes();

/// The theme named @p name, or nothing.
optionalRef<theme> findTheme(std::string_view name);

/// The default theme, which uses the terminal's own palette.
const theme& defaultTheme();

/// WCAG 2 contrast ratio between two RGB colors (1 to 21).
double contrastRatio(std::uint32_t a, std::uint32_t b);

}  // namespace tui
