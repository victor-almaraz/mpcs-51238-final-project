#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <proj/ir/ir.h>

#include <nlohmann/json.hpp>

using Catch::Matchers::ContainsSubstring;
using metadata::graphSpec;
using metadata::nodeSpec;
using metadata::pipeKind;
using metadata::pipeRef;

namespace {

const pipeRef osc{pipeKind::audio, 0};
const pipeRef cutoff{pipeKind::control, 1};

// sine -> svf -> audioOut, with the cutoff on a control pipe.
graphSpec sample() {
    graphSpec g;
    g.seed = 7;
    g.tempo = 96.0;
    g.meter = 3;
    g.pipes = {{osc, 2, false}, {cutoff, 1, false}, {{pipeKind::audio, 1}, 2, false}};
    nodeSpec sine{"main.lisp/piece/sine#0", "sineOsc", {{"freq", 440.0}}, {}, {{"out", {osc}}},
                  metadata::sourceSpan{"main.lisp", 3, 5, 40, 20}};
    nodeSpec filter{"main.lisp/piece/svf#0", "svf",
                    {{"cutoff", cutoff}, {"q", 0.7}, {"mode", std::string("low")}},
                    {{"in", {osc}}},
                    {{"out", {{pipeKind::audio, 1}}}},
                    std::nullopt};
    nodeSpec out{"main.lisp/piece/out#0", "audioOut", {}, {{"in", {{pipeKind::audio, 1}}}}, {}, std::nullopt};
    g.nodes = {sine, filter, out};
    return g;
}

std::string messages(const std::vector<ir::error>& errors) {
    std::string out;
    for (const auto& e : errors) out += e.text() + "\n";
    return out;
}

} // namespace

TEST_CASE("IR round trips through JSON") {
    const graphSpec g = sample();
    const auto text = ir::write(g, 2);
    const auto r = ir::read(text);
    INFO(r.text());
    REQUIRE(r.ok());
    CHECK(*r.graph == g);
    CHECK(ir::write(*r.graph) == ir::write(g));
}

TEST_CASE("the embedded schema is the schema file") {
    const auto schema = nlohmann::json::parse(ir::schema());
    CHECK(schema.at("$id") == "urn:proj:ir:1");
}

TEST_CASE("single pipe references and defaults read") {
    const auto r = ir::read(R"({"format":"proj-ir","version":1,
        "pipes":[{"pipe":{"audio":0}}],
        "nodes":[{"id":"a","type":"sineOsc","outputs":{"out":{"audio":0}}}]})");
    INFO(r.text());
    REQUIRE(r.ok());
    CHECK(r.graph->meter == 4);
    CHECK(r.graph->pipes[0].channels == 1);
    CHECK(r.graph->nodes[0].outputs.at("out") == std::vector<pipeRef>{osc});
}

TEST_CASE("schema errors name the location") {
    CHECK_THAT(ir::read("not json").text(), ContainsSubstring("not valid JSON"));
    CHECK_THAT(ir::read(R"({"format":"proj-ir","version":1,"pipes":[]})").text(), ContainsSubstring("nodes"));
    const auto r = ir::read(R"({"format":"proj-ir","version":1,"pipes":[],
        "nodes":[{"id":"n1","type":"sineOsc","colour":1}]})");
    REQUIRE_FALSE(r.ok());
    CHECK(r.errors[0].node == "n1");
    CHECK_THAT(r.errors[0].message, ContainsSubstring("/nodes/0"));
}

TEST_CASE("semantic errors identify the node and its source") {
    auto check = [](graphSpec g, const std::string& expected) {
        const auto errors = ir::validate(g);
        INFO(messages(errors));
        REQUIRE_FALSE(errors.empty());
        CHECK_THAT(messages(errors), ContainsSubstring(expected));
    };
    SECTION("unknown type") {
        auto g = sample();
        g.nodes[0].type = "theremin";
        check(g, "unknown primitive type theremin");
    }
    SECTION("unknown parameter") {
        auto g = sample();
        g.nodes[0].params["detune"] = 1.0;
        check(g, "unknown parameter detune");
    }
    SECTION("out of range, with the source span") {
        auto g = sample();
        g.nodes[0].params["freq"] = -5.0;
        check(g, "main.lisp:3:5: node main.lisp/piece/sine#0: parameter freq is -5");
    }
    SECTION("bad choice") {
        auto g = sample();
        g.nodes[1].params["mode"] = std::string("sideways");
        check(g, "must be one of: low, band, high, notch");
    }
    SECTION("static-only parameter bound to a pipe") {
        auto g = sample();
        g.nodes[2].params["channel"] = cutoff;
        check(g, "cannot be bound to a pipe");
    }
    SECTION("undeclared pipe") {
        auto g = sample();
        g.nodes[2].inputs["in"] = {{pipeKind::audio, 9}};
        check(g, "audio pipe 9 is not declared");
    }
    SECTION("wrong pipe type") {
        auto g = sample();
        g.nodes[2].inputs["in"] = {cutoff};
        check(g, "has the wrong type");
    }
    SECTION("missing required port") {
        auto g = sample();
        g.nodes[2].inputs.clear();
        check(g, "missing input in");
    }
    SECTION("one pipe per single port") {
        auto g = sample();
        g.nodes[2].inputs["in"] = {osc, osc};
        check(g, "takes one pipe");
    }
    SECTION("duplicate node ID") {
        auto g = sample();
        g.nodes[2].id = g.nodes[1].id;
        check(g, "duplicate node ID");
    }
    SECTION("reader before writer") {
        auto g = sample();
        std::swap(g.nodes[0], g.nodes[1]);
        check(g, "reads audio pipe 0 before node main.lisp/piece/sine#0 writes it");
    }
    SECTION("bad pipe declarations") {
        auto g = sample();
        g.pipes.push_back({{pipeKind::event, 0}, 2, false});
        g.pipes.push_back({{pipeKind::event, 1}, 1, true});
        g.pipes.push_back(g.pipes[0]);
        const auto text = messages(ir::validate(g));
        CHECK_THAT(text, ContainsSubstring("event pipes have one channel"));
        CHECK_THAT(text, ContainsSubstring("only audio and control pipes can be feedback pipes"));
        CHECK_THAT(text, ContainsSubstring("declared twice"));
    }
}

TEST_CASE("a cycle through a feedback pipe is in order") {
    graphSpec g;
    const pipeRef loop{pipeKind::audio, 0};
    const pipeRef out{pipeKind::audio, 1};
    g.pipes = {{loop, 1, true}, {out, 1, false}};
    // The delay reads the feedback pipe that the gain, later, writes.
    g.nodes.push_back({"delay", "delayLine", {}, {{"in", {loop}}}, {{"out", {out}}}, std::nullopt});
    g.nodes.push_back({"gain", "gain", {{"amount", 0.5}}, {{"in", {out}}}, {{"out", {loop}}}, std::nullopt});
    CHECK(ir::validate(g).empty());
    g.pipes[0].feedback = false;
    CHECK_FALSE(ir::validate(g).empty());
}

TEST_CASE("the tempo pipe needs no declaration") {
    graphSpec g;
    const pipeRef tempo{pipeKind::control, metadata::tempoPipe};
    g.nodes.push_back({"metro", "metro", {}, {}, {{"out", {{pipeKind::event, 0}}}}, std::nullopt});
    g.pipes = {{{pipeKind::event, 0}, 1, false}};
    g.nodes.push_back({"lfo", "lfo", {}, {}, {{"out", {tempo}}}, std::nullopt});
    CHECK(ir::validate(g).empty());
}
