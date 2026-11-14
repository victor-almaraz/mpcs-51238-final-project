#include <catch2/catch_test_macros.hpp>

#include <proj/metadata/primitiveSpec.h>

#include <algorithm>
#include <set>
#include <string>

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
    const primitiveSpec& sine = *found;
    const paramSpec& freq = *sine.param("freq");
    CHECK(freq.dim == dimension::frequency);
    CHECK(freq.defaultValue == 440.0);
    CHECK(contains(freq.bindable, pipeKind::audio));
    CHECK(sine.output("out")->get().required);
    CHECK_FALSE(sine.param("nope"));
    CHECK_FALSE(findPrimitive("nope"));
}

TEST_CASE("pipe kind names round trip") {
    for (auto k : {pipeKind::audio, pipeKind::control, pipeKind::event, pipeKind::video}) {
        CHECK(parsePipeKind(toString(k)) == k);
    }
    CHECK_FALSE(parsePipeKind("bus"));
}
