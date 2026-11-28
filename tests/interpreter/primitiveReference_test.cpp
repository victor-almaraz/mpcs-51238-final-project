// The primitive reference, docs/reference/primitives.md, written from the
// primitive tables: a test fails when the committed page differs from the
// tables, and the hidden `[.update-primitive-reference]` case, which the
// `primitive-reference` target runs, rewrites it.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/interpreter/units.h>
#include <espresso/metadata/graph.h>
#include <espresso/metadata/primitiveSpec.h>
#include <espresso/metadata/text.h>

#include "support/files.h"

namespace {

namespace fs = std::filesystem;
using namespace metadata;

struct group {
    std::string_view title;
    std::vector<std::string_view> types;
};

// The reference's sections, in order; every type of the tables is in one.
std::vector<group> groups() {
    return {
        {.title = "Sources",
         .types = {"audioIn", "midiIn", "oscIn", "serialIn", "webIn", "webStream", "webSocket"}},
        {.title = "Sinks", .types = {"audioOut", "midiOut", "oscOut", "bufferSave", "serialOut", "webOut"}},
        {.title = "Oscillators", .types = {"sineOsc", "sawOsc", "pulseOsc", "triangleOsc", "noise"}},
        {.title = "Filters", .types = {"biquad", "svf", "ladder"}},
        {.title = "Envelopes and modulation", .types = {"adsr", "lfo", "smoother"}},
        {.title = "Delays", .types = {"delayLine", "comb", "allpass"}},
        {.title = "Buffers",
         .types = {"bufferWrite", "bufferRead", "samplePlayer", "grains", "wavetableOsc"}},
        {.title = "Spectral", .types = {"fft", "ifft", "pvDelay", "pvFreeze", "pvBlur"}},
        {.title = "Math and routing",
         .types = {"add", "multiply", "mix", "gain", "clip", "pan", "crossfade"}},
        {.title = "Conversion", .types = {"dbToAmp", "beatsToSeconds"}},
        {.title = "Mapper operations",
         .types = {"neg", "abs", "exp", "log", "sin", "cos", "tanh", "floor", "round", "subtract", "divide",
                   "pow", "min", "max", "quantize", "clamp", "scale", "scaleExp"}},
        {.title = "Events",
         .types = {"noteToFreq", "gate", "velocity", "controlChange", "pitchBend", "eventFilter", "oscNotes",
                   "voiceSplit", "triggerNotes", "metro", "sequencer", "chance", "threshold", "sampleAndHold",
                   "clipPlayer"}},
    };
}

// Each item of @p items, as @p show writes it, separated by commas, with
// @p last before the final one.
template <typename T, typename Show>
std::string join(std::span<const T> items, Show show, std::string_view last = ", ") {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += i + 1 == items.size() ? last : ", ";
        out += show(items[i]);
    }
    return out;
}

std::string keyword(std::string_view name) { return "`:" + std::string(name) + "`"; }
std::string code(std::string_view text) { return "`" + std::string(text) + "`"; }

// A value of dimension @p d, in canonical units, as the language writes it.
std::string quantity(dimension d, double v) {
    switch (d) {
    case dimension::time:
        return v != 0.0 && std::abs(v) < 1.0 ? formatNumber(v * 1000.0) + " ms" : formatNumber(v) + " s";
    case dimension::frequency: return formatNumber(v) + " Hz";
    case dimension::level:
        if (v == 0.0) return "silence";
        return v > 0.0 ? formatNumber(std::round(interpreter::decibels(v) * 100.0) / 100.0) + " dB"
                       : formatNumber(v);
    case dimension::interval: return formatNumber(v) + " st";
    case dimension::beats: return formatNumber(v) + (v == 1.0 ? " beat" : " beats");
    default: return formatNumber(v);
    }
}

std::string kinds(kindSet set) {
    std::vector<std::string_view> named;
    for (const pipeKind k : {pipeKind::audio, pipeKind::control, pipeKind::event}) {
        if (contains(set, k)) named.push_back(toString(k));
    }
    return join(std::span<const std::string_view>(named), [](std::string_view k) { return std::string(k); });
}

std::string defaultOf(const paramSpec& p) {
    if (p.required) return "required";
    if (isNumeric(p.dim)) return quantity(p.dim, p.defaultValue);
    if (p.defaultText.empty()) return "";
    if (p.dim == dimension::choice) return keyword(p.defaultText);
    return p.dim == dimension::text ? code(p.defaultText) : "";
}

// The range a parameter's table entry allows, or its choices.
std::string rangeOf(const paramSpec& p) {
    if (p.dim == dimension::choice) return join(p.choices, keyword);
    if (!isNumeric(p.dim)) return "";
    const paramSpec unbounded{};
    const bool low = p.min > unbounded.min;
    const bool high = p.max < unbounded.max;
    if (low && high) return quantity(p.dim, p.min) + " to " + quantity(p.dim, p.max);
    if (low) return "from " + quantity(p.dim, p.min);
    if (high) return "up to " + quantity(p.dim, p.max);
    return "";
}

// The constraint of @p s of kind @p kind on parameter @p param, if there is one.
std::optional<constraint> ruleOn(const primitiveSpec& s, rule kind, std::string_view param) {
    for (const constraint& c : s.constraints) {
        if (c.kind == kind && c.param == param) return c;
    }
    return std::nullopt;
}

// The values parameter @p p of @p s takes: its range, narrowed by the constraints on it.
std::string valuesOf(const primitiveSpec& s, const paramSpec& p) {
    if (const auto only = ruleOn(s, rule::oneOf, p.name)) {
        return join(only->values, [&p](double v) { return quantity(p.dim, v); });
    }
    std::vector<std::string> parts{rangeOf(p)};
    if (parts.front().empty()) parts.clear();
    if (ruleOn(s, rule::powerOfTwo, p.name)) parts.emplace_back("a power of two");
    if (p.pagePort) parts.emplace_back("or `:page`");
    return join(std::span<const std::string>(parts), [](const std::string& part) { return part; });
}

// A sentence for each constraint the parameter table does not show.
std::string requirements(const primitiveSpec& s) {
    std::string out;
    const auto add = [&out](std::initializer_list<std::string_view> parts) {
        out += ' ';
        for (const std::string_view part : parts) out += part;
    };
    for (const constraint& c : s.constraints) {
        const std::string param = keyword(c.param);
        const std::string first = c.ports.empty() ? "" : keyword(c.ports[0]);
        switch (c.kind) {
        case rule::anyInput: add({"One of the inputs ", join(c.ports, keyword, " or "), " is bound."}); break;
        case rule::anyOutput:
            add({"One of the outputs ", join(c.ports, keyword, " or "), " is bound."});
            break;
        case rule::entryPerPipe:
            add({param, ", when it has entries, has one for each pipe of ", first, "."});
            break;
        case rule::notEmpty: add({param, " has an entry."}); break;
        case rule::channels:
            add({first, ", when bound, has a pipe of at least ", std::to_string(c.count), " channels."});
            break;
        case rule::audioWith:
            add({"With ", first, " bound, ", keyword(c.ports[1]), " is an audio pipe."});
            break;
        case rule::textWith: add({"With ", first, " bound, ", param, " is not empty."}); break;
        case rule::scheme: add({param, " starts with ", join(c.names, code, " or "), "."}); break;
        case rule::atMost: add({param, " has at most ", std::to_string(c.count), " entries."}); break;
        case rule::headers: add({"Each entry of ", param, " is a request header, `Name: value`."}); break;
        case rule::entryPerChannel:
            add({param, ", when it has entries, has one for each channel of ", first, ", when bound."});
            break;
        case rule::powerOfTwo:
        case rule::oneOf: break;  // in the Values column
        }
    }
    return out;
}

std::string portLine(const portSpec& port) {
    std::string out = keyword(port.name) + " (" + (port.frames ? std::string("frames") : kinds(port.kinds));
    if (port.required) out += ", required";
    if (port.multiple) out += ", several pipes";
    return out + ")";
}

std::string ports(std::span<const portSpec> list) { return list.empty() ? "none" : join(list, portLine); }

std::string_view roleName(role r) {
    switch (r) {
    case role::source: return "source";
    case role::sink: return "sink";
    case role::generator: return "generator";
    }
    return "generator";
}

constexpr std::string_view introduction =
    R"(Every primitive, by the name the language calls it by and its IR type. A parameter takes a constant, written in any unit of its dimension, or, where the table names pipe kinds, a pipe of one of those kinds. Defaults and ranges are in the units shown; levels are shown in decibels, and a level of 0 as silence. A time parameter that takes control pipes also takes a beat value, which follows the tempo. Ports connect pipes: inputs read them and outputs write them. The [language reference](../language.md) describes what the primitives do, and the [IR format](../ir.md) how they are written in the IR.)";

std::string reference() {
    std::ostringstream out;
    out << "<!-- Written from src/metadata/primitiveSpec.cpp by `cmake --build build --target "
           "primitive-reference`; "
           "tests/interpreter/primitiveReference_test.cpp fails when it differs. -->\n\n"
        << "# Primitives\n\n"
        << introduction << "\n";
    for (const group& g : groups()) {
        out << "\n## " << g.title << "\n";
        for (const std::string_view type : g.types) {
            const auto spec = findPrimitive(type);
            if (!spec) continue;
            const primitiveSpec& s = spec->get();
            out << "\n### " << interpreter::languageName(type) << "\n\n"
                << "IR type `" << type << "`, a " << roleName(s.kind) << ". Inputs: " << ports(s.inputs)
                << ". Outputs: " << ports(s.outputs) << "." << requirements(s) << "\n";
            if (s.params.empty()) continue;
            out << "\n| Parameter | Dimension | Default | Values | Pipes |\n|---|---|---|---|---|\n";
            for (const paramSpec& p : s.params) {
                out << "| `:" << p.name << "` | " << toString(p.dim) << " | " << defaultOf(p) << " | "
                    << valuesOf(s, p) << " | " << kinds(p.bindable) << " |\n";
            }
        }
    }
    return out.str();
}

fs::path pagePath() { return fs::path(ESPRESSO_SOURCE_DIR) / "docs" / "reference" / "primitives.md"; }

}  // namespace

TEST_CASE("every primitive type is in one section of the reference", "[primitive-reference]") {
    std::set<std::string_view> listed;
    for (const group& g : groups()) {
        for (const std::string_view type : g.types) {
            CHECK(findPrimitive(type).has_value());
            CHECK(listed.insert(type).second);
        }
    }
    for (const primitiveSpec& s : primitiveSpecs()) CHECK(listed.contains(s.type));
}

TEST_CASE("docs/reference/primitives.md is written from the primitive tables", "[primitive-reference]") {
    REQUIRE(fs::exists(pagePath()));
    CHECK(support::readText(pagePath()) == reference());
}

TEST_CASE("rewrite docs/reference/primitives.md", "[.update-primitive-reference]") {
    std::ofstream out(pagePath(), std::ios::binary);
    REQUIRE(out);
    out << reference();
    CHECK(out.good());
}
