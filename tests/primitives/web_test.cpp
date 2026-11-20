// webIn against frames delivered through the hub (io.md §5.1, §11.2).

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/event.h>
#include <espresso/primitives/io.h>

#include "support/graph.h"

using namespace support;
using namespace primitives;
using strings = std::vector<std::string>;

namespace {

constexpr double rate = 48000.0;
constexpr int block = 64;

dataFrame frameOf(std::initializer_list<float> values) {
    dataFrame f;
    for (const float v : values) f.values[f.count++] = v;
    return f;
}

// Two webIn nodes polling one URL for different fields, into 2-channel
// control 1 and control 2.
graph weather() {
    graph g;
    g.pipe(control(1), 2).pipe(control(2)).pipe(events(0));
    auto& now = g.node("now", "webIn");
    now.params = {{"url", std::string("https://api.example.com/now")},
                  {"every", 60.0},
                  {"fields", strings{"/main/temp", "/wind/speed"}},
                  {"headers", strings{"Accept: application/json"}}};
    now.outputs["value"] = {control(1)};
    now.outputs["out"] = {events(0)};
    auto& humid = g.node("humid", "webIn");
    humid.params = {{"url", std::string("https://api.example.com/now")},
                    {"fields", strings{"/main/humidity"}}};
    humid.outputs["value"] = {control(2)};
    return g;
}

}  // namespace

TEST_CASE("webIn ports carry each node's request for the web bridge (io.md 5.1)") {
    renderer r(1, rate, block);
    r.play(weather().spec);
    r.run(block);
    const auto ports = r.engine.io().webInputs(portKind::webIn);
    REQUIRE(ports.size() == 2);
    CHECK(ports[0]->owner() == "now");
    const webRequest& now = ports[0]->request();
    CHECK(now.url == "https://api.example.com/now");
    CHECK(now.fields == strings{"/main/temp", "/wind/speed"});
    CHECK(now.headers == strings{"Accept: application/json"});
    CHECK(now.every == 60.0);
    CHECK(now.timeout == 5.0);
    const webRequest& humid = ports[1]->request();
    CHECK(humid.every == 10.0);  // the defaults
    CHECK(humid.headers.empty());
    CHECK(r.engine.io().webInputs(portKind::serialIn).empty());
}

TEST_CASE("webIn writes the frames delivered to its node and an event per response") {
    renderer r(1, rate, block);
    r.play(weather().spec);
    r.run(block);
    r.engine.io().deliverWeb(portKind::webIn, "now", frameOf({21.5f, 3.0f}));
    r.engine.io().deliverWeb(portKind::webIn, "humid", frameOf({0.6f}));
    r.engine.io().deliverWeb(portKind::webIn, "nobody", frameOf({9.0f}));
    r.run(block);
    CHECK(r.engine.pipes().control(1).value(0) == 21.5f);
    CHECK(r.engine.pipes().control(1).value(1) == 3.0f);
    CHECK(r.engine.pipes().control(2).value(0) == 0.6f);
    const auto events = r.engine.pipes().events(0).events();
    REQUIRE(events.size() == 1);
    CHECK(number(events[0]) == 21.5f);

    // A swap keeps what was delivered but not yet read.
    r.engine.io().deliverWeb(portKind::webIn, "now", frameOf({22.0f}));
    r.play(weather().spec, 2);
    r.run(block);
    CHECK(r.engine.pipes().control(1).value(0) == 22.0f);
    CHECK(r.engine.pipes().control(1).value(1) == 3.0f);  // a channel the frame lacks
}
