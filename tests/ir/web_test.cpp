// Text lists and the web rows in the IR (io.md §5.1, §9.1, §9.3).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>

#include "support/pipes.h"
#include "support/unwrap.h"
#include "support/validation.h"

using Catch::Matchers::ContainsSubstring;
using metadata::graphSpec;
using metadata::paramValue;
using support::control;
using support::errors;
using support::events;
using strings = std::vector<std::string>;

namespace {

// A webIn polling @p url into a control pipe of @p channels channels,
// with @p params besides its URL.
graphSpec poll(std::map<std::string, paramValue, std::less<>> params, int channels = 2,
               std::string url = "https://api.example.com/now") {
    params.emplace("url", std::move(url));
    graphSpec g;
    g.pipes = {{.pipe = control(1), .channels = channels, .feedback = false}};
    g.nodes.push_back({.id = "w",
                       .type = "webIn",
                       .params = std::move(params),
                       .inputs = {},
                       .outputs = {{"value", {control(1)}}},
                       .src = std::nullopt});
    return g;
}

// An IR document of @p nodes, JSON objects that may use audio pipes 0
// and 1 and control pipe 1, of two channels.
std::string document(std::initializer_list<std::string> nodes) {
    std::string out =
        R"({"format":"espresso-ir","version":1,"pipes":[{"pipe":{"audio":0}},{"pipe":{"audio":1}},)"
        R"({"pipe":{"control":1},"channels":2}],"nodes":[)";
    for (const auto& n : nodes) out += (out.back() == '[' ? "" : ",") + n;
    return out + "]}";
}

// A sineOsc writing audio pipe 0, with @p params, the members of its
// parameter object.
std::string sine(const std::string& params = "") {
    return R"({"id":"s","type":"sineOsc","params":{)" + params + R"(},"outputs":{"out":{"audio":0}}})";
}

// A mix of audio pipe 0 into audio pipe 1 with levels @p levels.
std::string mix(const std::string& levels) {
    return R"({"id":"m","type":"mix","params":{"levels":)" + levels +
           R"(},"inputs":{"in":[{"audio":0}]},"outputs":{"out":{"audio":1}}})";
}

// A webIn polling into control pipe 1, with @p params, members of its
// parameter object besides its URL.
std::string web(const std::string& params) {
    return R"({"id":"w","type":"webIn","params":{"url":"https://api.example.com/now")" + params +
           R"(},"outputs":{"value":{"control":1}}})";
}

}  // namespace

TEST_CASE("a text list round trips as an array of strings and an empty one stays a text list") {
    const auto g = poll({{"fields", strings{"/main/temp", "/wind/speed"}}, {"headers", strings{}}});
    const auto text = ir::write(g);
    CHECK_THAT(text, ContainsSubstring(R"("fields":["/main/temp","/wind/speed"])"));
    CHECK_THAT(text, ContainsSubstring(R"("headers":[])"));
    const auto r = ir::read(text);
    INFO(r.text());
    REQUIRE(r.ok());
    CHECK(support::unwrap(r.graph) == g);
}

TEST_CASE("an empty array reads as the list its parameter takes") {
    const auto r = ir::read(document({sine(), mix("[]"), web(R"(,"fields":[],"headers":[])")}));
    INFO(r.text());
    REQUIRE(r.ok());
    const auto& g = support::unwrap(r.graph);
    CHECK(g.nodes[1].params.at("levels") == paramValue(std::vector<double>{}));
    CHECK(g.nodes[2].params.at("fields") == paramValue(strings{}));
    CHECK(g.nodes[2].params.at("headers") == paramValue(strings{}));
}

TEST_CASE("an array of the wrong type for its parameter reads by its type and fails validation") {
    const auto readErrors = [](std::initializer_list<std::string> nodes) {
        return ir::read(document(nodes)).text();
    };
    CHECK_THAT(readErrors({web(R"(,"fields":[1,2])")}),
               ContainsSubstring("node w: parameter fields must be a list of strings"));
    CHECK_THAT(readErrors({sine(), mix(R"(["a"])")}),
               ContainsSubstring("node m: parameter levels must be a list of numbers"));
    // Without a spec, [] reads as numbers, and validation names what is
    // unknown.
    CHECK_THAT(readErrors({sine(R"("bogus":[])")}),
               ContainsSubstring("node s: unknown parameter bogus for sineOsc"));
    CHECK_THAT(readErrors({R"({"id":"b","type":"bogusOsc","params":{"bogus":[]}})"}),
               ContainsSubstring("node b: unknown primitive type bogusOsc"));
    // The schema admits arrays of numbers and arrays of strings only.
    CHECK_THAT(readErrors({sine(R"("freq":[true])")}),
               ContainsSubstring("/nodes/0/params: validation failed for additional property 'freq'"));
}

TEST_CASE("a text list holds strings only") {
    CHECK_THAT(errors(poll({{"fields", std::vector<double>{1.0, 2.0}}})),
               ContainsSubstring("parameter fields must be a list of strings"));
    CHECK_THAT(errors(poll({{"fields", std::string("/main/temp")}})),
               ContainsSubstring("parameter fields must be a list of strings"));
    // The schema refuses an array that mixes strings and numbers.
    auto text = ir::write(poll({{"fields", strings{"/a", "/b"}}}));
    text.replace(text.find(R"("/b")"), 4, "2");
    CHECK_THAT(ir::read(text).text(),
               ContainsSubstring("/nodes/0/params: validation failed for additional property 'fields'"));
}

TEST_CASE("webIn checks its URL fields headers and interval (io.md 9.3)") {
    CHECK(errors(poll({{"fields", strings{"/a", "/b"}}, {"headers", strings{"Accept: application/json"}}}))
              .empty());
    CHECK(errors(poll({}, 1, "http://localhost:8080/")).empty());
    CHECK_THAT(errors(poll({}, 1, "ftp://example.com/")),
               ContainsSubstring("parameter url must start with http://, https://"));
    CHECK_THAT(errors(poll({}, 1, "")), ContainsSubstring("parameter url must not be empty"));
    graphSpec noUrl = poll({});
    noUrl.nodes[0].params.erase("url");
    CHECK_THAT(errors(noUrl), ContainsSubstring("parameter url is required"));

    CHECK_THAT(errors(poll({{"fields", strings(9, "/a")}}, 8)),
               ContainsSubstring("parameter fields has 9 entries; at most 8 are allowed"));
    CHECK_THAT(errors(poll({{"fields", strings{"/a", "/b", "/c"}}}, 2)),
               ContainsSubstring("parameter fields has 3 entries for 2 channels on output value"));
    CHECK(errors(poll({}, 2)).empty());  // no fields reads the whole document

    for (const std::string bad : {"Accept", ": value", "Accept:value"}) {
        INFO(bad);
        CHECK_THAT(
            errors(poll({{"headers", strings{bad}}})),
            ContainsSubstring("parameter headers entry \"" + bad + "\" must be a header, Name: value"));
    }
    CHECK_THAT(errors(poll({{"every", 0.05}})),
               ContainsSubstring("parameter every is 0.05; expected 0.1 to 3600"));
}

namespace {

// A node of @p type with @p params, @p inputs, and @p outputs, on control
// 1 (two channels) and events 0.
graphSpec one(std::string type, std::map<std::string, paramValue, std::less<>> params,
              std::map<std::string, std::vector<metadata::pipeRef>, std::less<>> inputs,
              std::map<std::string, std::vector<metadata::pipeRef>, std::less<>> outputs) {
    graphSpec g;
    g.pipes = {{.pipe = control(1), .channels = 2, .feedback = false},
               {.pipe = events(0), .channels = 1, .feedback = false}};
    g.nodes.push_back({.id = "n",
                       .type = std::move(type),
                       .params = std::move(params),
                       .inputs = std::move(inputs),
                       .outputs = std::move(outputs),
                       .src = std::nullopt});
    return g;
}

}  // namespace

TEST_CASE("webStream takes http URLs and webSocket ws URLs (io.md 9.3)") {
    const auto source = [](const std::string& type, const std::string& url) {
        return errors(one(type, {{"url", url}}, {}, {{"value", {control(1)}}}));
    };
    CHECK(source("webStream", "https://stream.wikimedia.org/v2/stream/recentchange").empty());
    CHECK_THAT(source("webStream", "wss://example.com/"),
               ContainsSubstring("parameter url must start with http://, https://"));
    CHECK(source("webSocket", "wss://jetstream2.us-east.bsky.network/subscribe").empty());
    CHECK(source("webSocket", "ws://localhost:9000/").empty());
    CHECK_THAT(source("webSocket", "https://example.com/"),
               ContainsSubstring("parameter url must start with ws://, wss://"));
    CHECK_THAT(errors(one("webSocket", {{"url", std::string("wss://a.example")}}, {}, {})),
               ContainsSubstring("needs at least one of output out, value"));
}

TEST_CASE("webOut names each value with a field and takes post put or get (io.md 7.1 and 9.3)") {
    const std::string url = "http://localhost:8080/level";
    CHECK(errors(one("webOut",
                     {{"url", url}, {"fields", strings{"left", "right"}}, {"method", std::string("put")}},
                     {{"in", {events(0)}}, {"value", {control(1)}}}, {}))
              .empty());
    CHECK(errors(one("webOut", {{"url", url}}, {{"in", {events(0)}}}, {})).empty());  // the event's number
    CHECK_THAT(errors(one("webOut", {{"url", url}}, {{"in", {events(0)}}, {"value", {control(1)}}}, {})),
               ContainsSubstring("with input value, parameter fields must not be empty"));
    CHECK_THAT(errors(one("webOut", {{"url", url}, {"fields", strings{"level"}}},
                          {{"in", {events(0)}}, {"value", {control(1)}}}, {})),
               ContainsSubstring("parameter fields has 1 entry for 2 channels on input value"));
    CHECK_THAT(
        errors(one("webOut", {{"url", url}, {"method", std::string("patch")}}, {{"in", {events(0)}}}, {})),
        ContainsSubstring("parameter method must be one of: post, put, get"));
    CHECK_THAT(errors(one("webOut", {{"url", url}}, {}, {})), ContainsSubstring("missing input in"));
    CHECK_THAT(errors(one("webOut", {{"url", std::string("ws://a.example")}}, {{"in", {events(0)}}}, {})),
               ContainsSubstring("parameter url must start with http://, https://"));
}
