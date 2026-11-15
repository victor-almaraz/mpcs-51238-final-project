#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <proj/ir/ir.h>
#include <proj/metadata/graph.h>

#include "support/unwrap.h"

using Catch::Matchers::ContainsSubstring;
using metadata::graphSpec;
using metadata::nodeSpec;
using metadata::pipeKind;
using metadata::pipeRef;

namespace {

const pipeRef osc{.kind = pipeKind::audio, .index = 0};
const pipeRef cutoff{.kind = pipeKind::control, .index = 1};

// sine -> svf -> audioOut, with the cutoff on a control pipe.
graphSpec sample() {
    graphSpec g;
    g.seed = 7;
    g.tempo = 96.0;
    g.meter = 3;
    g.pipes = {{.pipe = osc, .channels = 2, .feedback = false},
               {.pipe = cutoff, .channels = 1, .feedback = false},
               {.pipe = {.kind = pipeKind::audio, .index = 1}, .channels = 2, .feedback = false}};
    nodeSpec sine{
        .id = "main.lisp/piece/sine#0",
        .type = "sineOsc",
        .params = {{"freq", 440.0}},
        .inputs = {},
        .outputs = {{"out", {osc}}},
        .src = metadata::sourceSpan{.file = "main.lisp", .line = 3, .column = 5, .offset = 40, .length = 20}};
    nodeSpec filter{.id = "main.lisp/piece/svf#0",
                    .type = "svf",
                    .params = {{"cutoff", cutoff}, {"q", 0.7}, {"mode", std::string("low")}},
                    .inputs = {{"in", {osc}}},
                    .outputs = {{"out", {{.kind = pipeKind::audio, .index = 1}}}},
                    .src = std::nullopt};
    nodeSpec out{.id = "main.lisp/piece/out#0",
                 .type = "audioOut",
                 .params = {},
                 .inputs = {{"in", {{.kind = pipeKind::audio, .index = 1}}}},
                 .outputs = {},
                 .src = std::nullopt};
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
    CHECK(support::unwrap(r.graph) == g);
    CHECK(ir::write(support::unwrap(r.graph)) == ir::write(g));
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
    const auto& g = support::unwrap(r.graph);
    CHECK(g.meter == 4);
    CHECK(g.pipes[0].channels == 1);
    CHECK(g.nodes[0].outputs.at("out") == std::vector<pipeRef>{osc});
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

TEST_CASE("schema errors carry the node's source only when it is well formed") {
    const auto withSrc = [](const std::string& src) {
        return ir::read(R"({"format":"proj-ir","version":1,"pipes":[],
            "nodes":[{"id":"n1","type":"sineOsc","colour":1,"src":)" +
                        src + "}]}");
    };
    const auto good = withSrc(R"({"file":"a.lisp","line":3,"column":5})");
    REQUIRE_FALSE(good.ok());
    CHECK(good.errors[0].node == "n1");
    const auto& span = support::unwrap(good.errors[0].src);
    CHECK(span.file == "a.lisp");
    CHECK(span.line == 3);
    for (const std::string bad :
         {R"("a.lisp")", R"({"file":"a.lisp","line":"3","column":5})", R"({"file":"a.lisp","column":5})",
          R"({"file":"a.lisp","line":3,"column":5,"offset":"x"})"}) {
        INFO(bad);
        const auto r = withSrc(bad);
        REQUIRE_FALSE(r.ok());
        for (const auto& e : r.errors) {
            CHECK(e.node == "n1");
            CHECK_FALSE(e.src);
        }
    }
}

TEST_CASE("malformed documents are reported instead of thrown") {
    const std::string head = R"({"format":"proj-ir","version":1,)";
    const std::vector<std::string> bodies{
        R"("pipes":[],"nodes":{"x":{"id":1}}})",
        R"("pipes":[],"nodes":{"0":{"id":"n1"}}})",
        R"("pipes":[],"nodes":[5,"n",null]})",
        R"("pipes":[{"pipe":{"sound":0}}],"nodes":[]})",
        R"("pipes":[{"pipe":{}}],"nodes":[]})",
        R"("pipes":[],"nodes":[{"id":"n1","type":"sineOsc","outputs":{"out":[{"audio":0,"video":1}]}}]})",
        R"("pipes":[],"nodes":[{"id":"n1","type":"sineOsc","src":{"file":1,"line":1.5,"column":-1}}]})",
    };
    for (const auto& body : bodies) {
        INFO(body);
        ir::readResult r;
        CHECK_NOTHROW(r = ir::read(head + body));
        CHECK_FALSE(r.ok());
    }
}

TEST_CASE("semantic errors identify the node and its source") {
    auto check = [](const graphSpec& g, const std::string& expected) {
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
        g.nodes[2].inputs["in"] = {{.kind = pipeKind::audio, .index = 9}};
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
    SECTION("a node reading the pipe it writes") {
        auto g = sample();
        g.nodes[1].outputs["out"] = {osc};
        check(g, "reads audio pipe 0, which it writes");
    }
    SECTION("bad pipe declarations") {
        auto g = sample();
        g.pipes.push_back({.pipe = {.kind = pipeKind::event, .index = 0}, .channels = 2, .feedback = false});
        g.pipes.push_back({.pipe = {.kind = pipeKind::event, .index = 1}, .channels = 1, .feedback = true});
        g.pipes.push_back(g.pipes[0]);
        const auto text = messages(ir::validate(g));
        CHECK_THAT(text, ContainsSubstring("event pipes have one channel"));
        CHECK_THAT(text, ContainsSubstring("only audio and control pipes can be feedback pipes"));
        CHECK_THAT(text, ContainsSubstring("declared twice"));
    }
}

TEST_CASE("a cycle through a feedback pipe is in order") {
    graphSpec g;
    const pipeRef loop{.kind = pipeKind::audio, .index = 0};
    const pipeRef out{.kind = pipeKind::audio, .index = 1};
    g.pipes = {{.pipe = loop, .channels = 1, .feedback = true},
               {.pipe = out, .channels = 1, .feedback = false}};
    // The delay reads the feedback pipe that the gain, later, writes.
    g.nodes.push_back({.id = "delay",
                       .type = "delayLine",
                       .params = {},
                       .inputs = {{"in", {loop}}},
                       .outputs = {{"out", {out}}},
                       .src = std::nullopt});
    g.nodes.push_back({.id = "gain",
                       .type = "gain",
                       .params = {{"amount", 0.5}},
                       .inputs = {{"in", {out}}},
                       .outputs = {{"out", {loop}}},
                       .src = std::nullopt});
    CHECK(ir::validate(g).empty());
    g.pipes[0].feedback = false;
    CHECK_FALSE(ir::validate(g).empty());
}

TEST_CASE("the tempo pipe needs no declaration") {
    graphSpec g;
    const pipeRef tempo{.kind = pipeKind::control, .index = metadata::tempoPipe};
    g.nodes.push_back({.id = "metro",
                       .type = "metro",
                       .params = {},
                       .inputs = {},
                       .outputs = {{"out", {{.kind = pipeKind::event, .index = 0}}}},
                       .src = std::nullopt});
    g.pipes = {{.pipe = {.kind = pipeKind::event, .index = 0}, .channels = 1, .feedback = false}};
    g.nodes.push_back({.id = "lfo",
                       .type = "lfo",
                       .params = {},
                       .inputs = {},
                       .outputs = {{"out", {tempo}}},
                       .src = std::nullopt});
    CHECK(ir::validate(g).empty());
}

TEST_CASE("sortByDataFlow orders writers first and reports cycles") {
    auto g = sample();
    std::ranges::reverse(g.nodes);
    CHECK(ir::sortByDataFlow(g).empty());
    CHECK(g.nodes[0].type == "sineOsc");
    CHECK(g.nodes[2].type == "audioOut");
    CHECK(ir::validate(g).empty());

    auto cyclic = sample();
    cyclic.nodes[0].inputs["in"] = {{.kind = pipeKind::audio, .index = 1}};  // sine reads the filter's output
    cyclic.nodes[0].type = "gain";
    const auto before = cyclic;
    CHECK(ir::sortByDataFlow(cyclic) == std::vector<std::size_t>{0, 1});  // not audioOut, downstream
    CHECK(cyclic == before);
}

TEST_CASE("hazards report loop gain, large gains, and aliasing") {
    graphSpec g;
    const pipeRef fb{.kind = pipeKind::audio, .index = 0}, s{.kind = pipeKind::audio, .index = 1},
        x{.kind = pipeKind::audio, .index = 2};
    g.pipes = {{.pipe = fb, .channels = 1, .feedback = true},
               {.pipe = s, .channels = 1, .feedback = false},
               {.pipe = x, .channels = 1, .feedback = false}};
    g.nodes.push_back({.id = "n",
                       .type = "sineOsc",
                       .params = {{"freq", 30000.0}},
                       .inputs = {},
                       .outputs = {{"out", {s}}},
                       .src = std::nullopt});
    g.nodes.push_back({.id = "a",
                       .type = "add",
                       .params = {},
                       .inputs = {{"in", {s, fb}}},
                       .outputs = {{"out", {x}}},
                       .src = std::nullopt});
    g.nodes.push_back({.id = "g",
                       .type = "gain",
                       .params = {{"amount", 2.0}},
                       .inputs = {{"in", {x}}},
                       .outputs = {{"out", {fb}}},
                       .src = std::nullopt});
    const auto found = ir::hazards(g, 48000.0);
    REQUIRE(found.size() == 2);
    CHECK(found[0].node == "g");
    CHECK_THAT(found[0].message, ContainsSubstring("has a gain of 2"));
    CHECK(found[1].node == "n");
    CHECK_THAT(found[1].message, ContainsSubstring("above half the 48000 Hz sample rate"));
    g.nodes[2].params["amount"] = 0.9;
    g.nodes[0].params["freq"] = 440.0;
    CHECK(ir::hazards(g, 48000.0).empty());
}
