// The primitive reference, docs/reference/primitives.md, written from the
// primitive tables: a test fails when the committed page differs from the
// tables, and the hidden `[.update-primitive-reference]` case, which the
// `primitive-reference` target runs, rewrites it.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iterator>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/metadata/graph.h>
#include <espresso/metadata/primitiveSpec.h>

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

// The tables' "no limit" bounds.
constexpr double unbounded = 1e9;

std::string number(double v) {
    std::ostringstream out;
    out << std::setprecision(6) << v;
    return out.str();
}

// A value of dimension @p d, in canonical units, as the language writes it.
std::string quantity(dimension d, double v) {
    switch (d) {
    case dimension::time:
        return v != 0.0 && std::abs(v) < 1.0 ? number(v * 1000.0) + " ms" : number(v) + " s";
    case dimension::frequency: return number(v) + " Hz";
    case dimension::level:
        return v > 0.0 ? number(std::round(20.0 * std::log10(v) * 100.0) / 100.0) + " dB" : number(v);
    case dimension::interval: return number(v) + " st";
    case dimension::beats: return number(v) + (v == 1.0 ? " beat" : " beats");
    default: return number(v);
    }
}

std::string kinds(kindSet set) {
    std::string out;
    for (const pipeKind k : {pipeKind::audio, pipeKind::control, pipeKind::event}) {
        if ((set & bit(k)) == 0) continue;
        if (!out.empty()) out += ", ";
        out += toString(k);
    }
    return out;
}

std::string defaultOf(const paramSpec& p) {
    if (p.required) return "required";
    if (isNumeric(p.dim)) return quantity(p.dim, p.defaultValue);
    if (p.dim == dimension::choice || p.dim == dimension::text) {
        return p.defaultText.empty() ? "" : "`" + std::string(p.defaultText) + "`";
    }
    return "";
}

std::string valuesOf(const paramSpec& p) {
    if (p.dim == dimension::choice) {
        std::string out;
        for (const std::string_view c : p.choices)
            out += (out.empty() ? "`:" : ", `:") + std::string(c) + "`";
        return out;
    }
    if (!isNumeric(p.dim)) return "";
    const bool low = p.min > -unbounded;
    const bool high = p.max < unbounded;
    if (low && high) return quantity(p.dim, p.min) + " to " + quantity(p.dim, p.max);
    if (low) return "from " + quantity(p.dim, p.min);
    if (high) return "up to " + quantity(p.dim, p.max);
    return "";
}

std::string portLine(const portSpec& port) {
    std::string out =
        "`:" + std::string(port.name) + "` (" + (port.frames ? std::string("frames") : kinds(port.kinds));
    if (port.required) out += ", required";
    if (port.multiple) out += ", several pipes";
    return out + ")";
}

std::string ports(std::span<const portSpec> list) {
    std::string out;
    for (const portSpec& port : list) out += (out.empty() ? "" : ", ") + portLine(port);
    return out.empty() ? "none" : out;
}

std::string_view roleName(role r) {
    switch (r) {
    case role::source: return "source";
    case role::sink: return "sink";
    default: return "generator";
    }
}

std::string reference() {
    std::ostringstream out;
    out << "<!-- Written from src/metadata/primitiveSpec.cpp by `cmake --build build --target "
           "primitive-reference`; "
           "tests/interpreter/primitiveReference_test.cpp fails when it differs. -->\n\n"
        << "# Primitives\n\n"
        << "Every primitive, by the name the language calls it by and its IR type. A parameter takes a "
           "constant, "
           "written in any unit of its dimension, or, where the table names pipe kinds, a pipe of one of "
           "those kinds. "
           "Defaults and ranges are in the units shown; levels are shown in decibels. Ports connect pipes: "
           "inputs "
           "read them and outputs write them. The [language reference](../language.md) describes what the "
           "primitives "
           "do, and the [IR format](../ir.md) how they are written in the IR.\n";
    for (const group& g : groups()) {
        out << "\n## " << g.title << "\n";
        for (const std::string_view type : g.types) {
            const auto spec = findPrimitive(type);
            if (!spec) continue;
            const primitiveSpec& s = spec->get();
            out << "\n### " << interpreter::languageName(type) << "\n\n"
                << "IR type `" << type << "`, a " << roleName(s.kind) << ". Inputs: " << ports(s.inputs)
                << ". Outputs: " << ports(s.outputs) << ".\n";
            if (s.params.empty()) continue;
            out << "\n| Parameter | Dimension | Default | Values | Pipes |\n|---|---|---|---|---|\n";
            for (const paramSpec& p : s.params) {
                out << "| `:" << p.name << "` | " << toString(p.dim) << " | " << defaultOf(p) << " | "
                    << valuesOf(p) << " | " << kinds(p.bindable) << " |\n";
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
    std::ifstream in(pagePath(), std::ios::binary);
    REQUIRE(in);
    const std::string committed{std::istreambuf_iterator<char>(in), {}};
    CHECK(committed == reference());
}

TEST_CASE("rewrite docs/reference/primitives.md", "[.update-primitive-reference]") {
    std::ofstream out(pagePath(), std::ios::binary);
    REQUIRE(out);
    out << reference();
    CHECK(out.good());
}
