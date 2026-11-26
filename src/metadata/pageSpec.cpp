// The kinds of a page's layout (dsl-ui.md §7.1): each one's parameters,
// as the es-* elements that draw them take them (ui.md §6, docs/language.md,
// Page controls and Page layout). Values are in canonical units; a unit
// names how a control shows its value.

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

#include <espresso/metadata/graph.h>
#include <espresso/metadata/pageSpec.h>
#include <espresso/metadata/primitiveSpec.h>

namespace metadata {

namespace {

constexpr double big = 1e9;

constexpr paramSpec number(std::string_view name, double fallback, double min = -big, double max = big) {
    return {.name = name, .dim = dimension::number, .defaultValue = fallback, .min = min, .max = max};
}
constexpr paramSpec choice(std::string_view name, std::span<const std::string_view> names,
                           std::string_view fallback) {
    return {.name = name, .dim = dimension::choice, .choices = names, .defaultText = fallback};
}
constexpr paramSpec list(std::string_view name) { return {.name = name, .dim = dimension::list}; }
constexpr paramSpec names(std::string_view name, bool required = false) {
    return {.name = name, .dim = dimension::textList, .required = required};
}

constexpr std::array<std::string_view, 2> flows = {"row", "column"};
constexpr std::array<std::string_view, 3> scales = {"linear", "exp", "db"};
constexpr std::array<std::string_view, 4> units = {"hz", "s", "db", "%"};
constexpr std::array<std::string_view, 2> sizes = {"standard", "big"};
constexpr std::array<std::string_view, 2> orientations = {"horizontal", "vertical"};

// A value's range (ui.md §5.2).
constexpr paramSpec min = number("min", 0);
constexpr paramSpec max = number("max", 1);
constexpr paramSpec scale = choice("scale", scales, "linear");
constexpr paramSpec step = number("step", 0, 0);
constexpr paramSpec unit = choice("unit", units, "");
constexpr paramSpec options = names("options");
constexpr paramSpec orientation = choice("orientation", orientations, "horizontal");

constexpr std::array placement = {list("span"), list("at")};

constexpr std::array<paramSpec, 0> none{};
constexpr std::array panelParams = {choice("flow", flows, "row")};
constexpr std::array sectionParams = {number("columns", 0, 1, 64)};
constexpr std::array knobParams = {min,
                                   max,
                                   scale,
                                   step,
                                   number("initial", 0),
                                   unit,
                                   options,
                                   choice("size", sizes, "standard"),
                                   number("sweep", 135, 1, 180),
                                   list("angles"),
                                   number("steps", 0, 2, 64)};
constexpr std::array sliderParams = {min, max, scale, step, number("initial", 0), unit, options, orientation};
constexpr std::array toggleParams = {number("initial", 0, 0, 1), orientation};
constexpr std::array selectParams = {names("options", true), number("initial", 0, 0)};
constexpr std::array xyParams = {min, max, scale, step, list("initial"), unit};
constexpr std::array keysParams = {number("low", 48, 0, 127), number("high", 71, 0, 127),
                                   number("velocity", 100, 1, 127)};
constexpr std::array meterParams = {
    min, max, scale, paramSpec{.name = "hold", .dim = dimension::time, .defaultValue = 1, .min = 0},
    orientation};
constexpr std::array lampParams = {min, max};
constexpr std::array valueParams = {unit, options};

constexpr std::array<std::string_view, 1> panels = {"panel"};
constexpr std::array<std::string_view, 1> sections = {"section"};
constexpr std::array<std::string_view, 10> controls = {"knob", "slider", "toggle", "button", "select",
                                                       "xy",   "keys",   "meter",  "lamp",   "value"};

constexpr auto group = pageRole::group;
constexpr auto sends = pageRole::sends;
constexpr auto shows = pageRole::shows;

constexpr std::array kinds = {
    pageKindSpec{.kind = pageRoot, .role = group, .element = "", .params = none, .children = panels},
    pageKindSpec{
        .kind = "panel", .role = group, .element = "es-panel", .params = panelParams, .children = sections},
    pageKindSpec{.kind = "section",
                 .role = group,
                 .element = "es-section",
                 .params = sectionParams,
                 .children = controls},
    pageKindSpec{.kind = "knob", .role = sends, .element = "es-knob", .params = knobParams},
    pageKindSpec{.kind = "slider", .role = sends, .element = "es-slider", .params = sliderParams},
    pageKindSpec{.kind = "toggle", .role = sends, .element = "es-toggle", .params = toggleParams},
    pageKindSpec{.kind = "button", .role = sends, .element = "es-button", .params = none},
    pageKindSpec{.kind = "select", .role = sends, .element = "es-select", .params = selectParams},
    pageKindSpec{.kind = "xy", .role = sends, .element = "es-xy", .params = xyParams},
    pageKindSpec{
        .kind = "keys", .role = sends, .element = "es-keys", .params = keysParams, .defaultAddress = "/note"},
    pageKindSpec{.kind = "meter", .role = shows, .element = "es-meter", .params = meterParams},
    pageKindSpec{.kind = "lamp", .role = shows, .element = "es-lamp", .params = lampParams},
    pageKindSpec{.kind = "value", .role = shows, .element = "es-value", .params = valueParams},
};

}  // namespace

optionalRef<paramSpec> pageKindSpec::param(std::string_view name) const noexcept {
    for (const auto set : {params, std::span<const paramSpec>(placement)}) {
        const auto it = std::ranges::find(set, name, &paramSpec::name);
        if (it != set.end()) return std::cref(*it);
    }
    return std::nullopt;
}

std::span<const pageKindSpec> pageKinds() noexcept { return kinds; }

optionalRef<pageKindSpec> findPageKind(std::string_view kind) noexcept {
    const auto it = std::ranges::find(kinds, kind, &pageKindSpec::kind);
    if (it == kinds.end()) return std::nullopt;
    return std::cref(*it);
}

std::span<const paramSpec> placementParams() noexcept { return placement; }

}  // namespace metadata
