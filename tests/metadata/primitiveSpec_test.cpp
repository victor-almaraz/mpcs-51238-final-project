#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/metadata/primitiveSpec.h>

#include "support/unwrap.h"

using namespace metadata;

TEST_CASE("primitive types and their parameter and port names are unique") {
    std::set<std::string_view> types;
    for (const auto& spec : primitiveSpecs()) {
        INFO(spec.type);
        CHECK(types.insert(spec.type).second);
        std::set<std::string_view> names;
        for (const auto& p : spec.params) CHECK(names.insert(p.name).second);
        std::set<std::string_view> inputs;
        for (const auto& p : spec.inputs) CHECK(inputs.insert(p.name).second);
        std::set<std::string_view> outputs;
        for (const auto& p : spec.outputs) CHECK(outputs.insert(p.name).second);
    }
}

TEST_CASE("parameter defaults are valid values") {
    for (const auto& spec : primitiveSpecs()) {
        for (const auto& p : spec.params) {
            INFO(std::string(spec.type) + " " + std::string(p.name));
            if (p.dim == dimension::choice) {
                REQUIRE_FALSE(p.choices.empty());
                CHECK(p.defaultText == p.choices.front());
            } else if (p.dim != dimension::text && p.dim != dimension::list) {
                CHECK(p.min <= p.defaultValue);
                CHECK(p.defaultValue <= p.max);
            }
        }
    }
}

TEST_CASE("sources have outputs, sinks have inputs") {
    for (const auto& spec : primitiveSpecs()) {
        INFO(spec.type);
        if (spec.kind == role::source) CHECK_FALSE(spec.outputs.empty());
        if (spec.kind == role::sink) CHECK_FALSE(spec.inputs.empty());
        if (spec.kind == role::generator) CHECK_FALSE(spec.outputs.empty());
    }
}

TEST_CASE("lookup by type, parameter, and port") {
    const auto found = findPrimitive("sineOsc");
    REQUIRE(found);
    const primitiveSpec& sine = support::unwrap(found);
    const paramSpec& freq = support::unwrap(sine.param("freq"));
    CHECK(freq.dim == dimension::frequency);
    CHECK(freq.defaultValue == 440.0);
    CHECK(contains(freq.bindable, pipeKind::audio));
    CHECK(support::unwrap(sine.output("out")).get().required);
    CHECK_FALSE(sine.param("nope"));
    CHECK_FALSE(findPrimitive("nope"));
}

TEST_CASE("pipe kind names round trip") {
    for (auto k : {pipeKind::audio, pipeKind::control, pipeKind::event, pipeKind::video}) {
        CHECK(parsePipeKind(toString(k)) == k);
    }
    CHECK_FALSE(parsePipeKind("bus"));
}

TEST_CASE("mapper operations read in and take their operands as parameters") {
    std::set<std::string_view> ops;
    for (const auto& spec : primitiveSpecs()) {
        if (spec.mapper == operands::none) continue;
        INFO(spec.type);
        ops.insert(spec.type);
        REQUIRE(spec.inputs.size() == 1);
        CHECK(spec.inputs[0].name == "in");
        CHECK(spec.params.size() <= 4);
        if (spec.mapper == operands::ranges) CHECK(spec.params.size() == 4);
    }
    // primitives.md §7.9, less the arithmetic, which lowers to other types.
    CHECK(ops == std::set<std::string_view>{"neg", "abs", "scale", "scaleExp", "clamp", "min", "max", "pow",
                                            "exp", "log", "sin", "cos", "tanh", "floor", "round",
                                            "quantize"});
}

TEST_CASE("constraints name parameters and ports of their type") {
    for (const auto& spec : primitiveSpecs()) {
        for (const auto& c : spec.constraints) {
            INFO(spec.type);
            if (!c.param.empty()) CHECK(spec.param(c.param));
            for (std::size_t i = 0; i < c.ports.size(); ++i) {
                if (c.kind == rule::entryPerChannel) {  // an input or an output
                    CHECK((spec.output(c.ports[i]) || spec.input(c.ports[i])));
                    continue;
                }
                const bool output = c.kind == rule::anyOutput || c.kind == rule::channels ||
                                    (c.kind == rule::audioWith && i == 1);
                CHECK((output ? spec.output(c.ports[i]) : spec.input(c.ports[i])));
            }
        }
    }
}

TEST_CASE("constraint messages use the requested names") {
    const primitiveSpec& mix = support::unwrap(findPrimitive("mix"));
    graphSpec g;
    g.pipes = {{.pipe = {.kind = pipeKind::audio, .index = 0}, .channels = 1, .feedback = false},
               {.pipe = {.kind = pipeKind::audio, .index = 1}, .channels = 1, .feedback = false}};
    nodeSpec n{.id = "m",
               .type = "mix",
               .params = {{"levels", std::vector<double>{1.0, 2.0, 3.0}}},
               .inputs = {{"in", {{.kind = pipeKind::audio, .index = 0}}}},
               .outputs = {{"out", {{.kind = pipeKind::audio, .index = 1}}}},
               .src = std::nullopt};
    CHECK(checkConstraints(mix, n, g, naming::ir) ==
          std::vector<std::string>{"parameter levels has 3 entries for 1 pipe on input in"});
    CHECK(checkConstraints(mix, n, g, naming::language) ==
          std::vector<std::string>{":levels has 3 entries for 1 pipe on :in"});
    n.params["levels"] = std::vector<double>{1.0};
    CHECK(checkConstraints(mix, n, g, naming::ir).empty());
}

TEST_CASE("writers by pipe, with and without feedback pipes") {
    graphSpec g;
    const pipeRef a{.kind = pipeKind::audio, .index = 0}, fb{.kind = pipeKind::audio, .index = 1};
    g.pipes = {{.pipe = a, .channels = 1, .feedback = false}, {.pipe = fb, .channels = 1, .feedback = true}};
    g.nodes = {{.id = "x",
                .type = "sineOsc",
                .params = {},
                .inputs = {},
                .outputs = {{"out", {a}}},
                .src = std::nullopt},
               {.id = "y",
                .type = "gain",
                .params = {},
                .inputs = {{"in", {a}}},
                .outputs = {{"out", {fb}}},
                .src = std::nullopt},
               {.id = "z",
                .type = "sineOsc",
                .params = {},
                .inputs = {},
                .outputs = {{"out", {a}}},
                .src = std::nullopt}};
    const auto all = writersByPipe(g, false);
    CHECK(all.at(a) == std::vector<std::size_t>{0, 2});
    CHECK(all.at(fb) == std::vector<std::size_t>{1});
    CHECK_FALSE(writersByPipe(g, true).contains(fb));
}

TEST_CASE("a pipe index finds the declaration findPipe finds") {
    graphSpec g;
    const pipeRef twice{.kind = pipeKind::audio, .index = 5};
    g.pipes = {{.pipe = {.kind = pipeKind::event, .index = 2}, .channels = 1, .feedback = false},
               {.pipe = twice, .channels = 2, .feedback = false},
               {.pipe = {.kind = pipeKind::control, .index = 1}, .channels = 3, .feedback = false},
               {.pipe = {.kind = pipeKind::audio, .index = 0}, .channels = 4, .feedback = true},
               {.pipe = twice, .channels = 8, .feedback = false}};
    const pipeIndex indexed(g);
    // Of two declarations of one pipe, the first, as findPipe finds it.
    CHECK(support::unwrap(indexed.find(twice)).get().channels == 2);
    CHECK(support::unwrap(findPipe(g, twice)).get().channels == 2);
    for (const auto& d : g.pipes) {
        INFO(toString(d.pipe));
        CHECK(support::unwrap(indexed.find(d.pipe)).get() == support::unwrap(findPipe(g, d.pipe)).get());
    }
    CHECK_FALSE(indexed.find({.kind = pipeKind::audio, .index = 1}));
    CHECK_FALSE(indexed.find({.kind = pipeKind::audio, .index = 6}));
    CHECK_FALSE(indexed.find({.kind = pipeKind::video, .index = 0}));
    CHECK_FALSE(indexed.find({.kind = pipeKind::control, .index = tempoPipe}));
    const graphSpec empty;
    CHECK_FALSE(pipeIndex(empty).find({.kind = pipeKind::audio, .index = 0}));
}

TEST_CASE("a pipe index agrees with findPipe on many pipes declared out of order") {
    // 300 pipes, 75 of each kind, in a scrambled order, with gaps in each
    // kind's indices.
    constexpr int count = 300;
    constexpr std::array kinds{pipeKind::audio, pipeKind::control, pipeKind::event, pipeKind::video};
    graphSpec g;
    for (int i = 0; i < count; ++i) {
        const int k = (i * 7) % count;  // 7 and 300 are coprime: each k once
        g.pipes.push_back({.pipe = {.kind = kinds.at(static_cast<std::size_t>(k % 4)), .index = 2 * (k / 4)},
                           .channels = 1 + (k % maxChannels),
                           .feedback = k % 3 == 0});
    }
    const pipeIndex indexed(g);
    for (const auto kind : kinds) {
        for (std::int32_t i = 0; i < 2 * count / 4 + 2; ++i) {
            const pipeRef r{.kind = kind, .index = i};
            INFO(toString(r));
            const auto expected = findPipe(g, r);
            const auto found = indexed.find(r);
            REQUIRE(found.has_value() == expected.has_value());
            if (expected) CHECK(support::unwrap(found).get() == support::unwrap(expected).get());
        }
    }
}

// A view into a temporary would dangle, so the rvalue overloads are deleted.
template <typename V>
concept viewable = requires(V&& v) { as<double>(std::forward<V>(v)); };
template <typename G>
concept searchable = requires(G&& g) { findPipe(std::forward<G>(g), pipeRef{}); };
static_assert(viewable<const paramValue&>);
static_assert(!viewable<paramValue>);
static_assert(searchable<const graphSpec&>);
static_assert(!searchable<graphSpec>);
static_assert(std::is_constructible_v<pipeIndex, const graphSpec&>);
static_assert(!std::is_constructible_v<pipeIndex, graphSpec>);
