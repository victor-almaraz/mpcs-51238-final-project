/// @file
/// The kinds of a page's layout (`dsl-ui.md` §7.1): groups, which hold
/// other kinds, and controls, each bound to an OSC address on the page
/// port. The table is data: the IR validator checks a layout against it,
/// and the interpreter and the page renderer read it.

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include <espresso/metadata/graph.h>
#include <espresso/metadata/primitiveSpec.h>

namespace metadata {

/// What an element of a layout is.
enum class pageRole : std::uint8_t {
    group,  ///< Holds other elements, of the kinds its entry names
    sends,  ///< A control: sends to its address, which an `oscIn` on the page port reads
    shows,  ///< A display: shows what an `oscOut` on the page port sends to its address
};

/// The table entry for one kind of layout element.
struct pageKindSpec {
    /// The kind, as the IR names it: `section`, `knob`.
    std::string_view kind;
    /// Group, control, or display.
    pageRole role = pageRole::group;
    /// The custom element that draws it: `es-section`, `es-knob`.
    std::string_view element;
    /// Its parameters, besides the placement every element but the root
    /// takes (`placementParams()`).
    std::span<const paramSpec> params;
    /// For a group, the kinds it holds.
    std::span<const std::string_view> children = noNames;
    /// For a control or a display, the address it takes when it gives none;
    /// empty when it MUST give one.
    std::string_view defaultAddress = noText;

    /// The parameter named @p name, among its own and the placement
    /// parameters, if there is one.
    optionalRef<paramSpec> param(std::string_view name) const noexcept;
};

/// Every kind of layout element: the groups, then the controls, then the
/// displays. A group's first kind that holds an element is the one an
/// element it cannot hold is wrapped in.
std::span<const pageKindSpec> pageKinds() noexcept;

/// The entry for @p kind, if there is one.
optionalRef<pageKindSpec> findPageKind(std::string_view kind) noexcept;

/// The kind of a layout's root: `page`.
inline constexpr std::string_view pageRoot = "page";

/// True when @p text is a color a page's `accent` may name: `#` and 3, 4,
/// 6, or 8 hexadecimal digits, or a CSS color's name in lowercase letters.
bool isPageColor(std::string_view text) noexcept;

/// The parameters of an element's place in its section (`dsl-ui.md`
/// §3.3), which every element but the root takes: `span`, its width and
/// height in units, and `at`, its column and row from 1. Each is a list of
/// two whole numbers above 0.
std::span<const paramSpec> placementParams() noexcept;

}  // namespace metadata
