// Web sources and webOut against frames delivered and drained through
// the hub (io.md §5–§7, §11.2).

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
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

TEST_CASE("webStream and webSocket ports carry their event and hello (io.md 6.1 and 6.4)") {
    graph g;
    g.pipe(control(1)).pipe(events(0));
    auto& stream = g.node("edits", "webStream");
    stream.params = {{"url", std::string("https://stream.example.com/")}, {"event", std::string("edit")}};
    stream.outputs["out"] = {events(0)};
    auto& socket = g.node("posts", "webSocket");
    socket.params = {{"url", std::string("wss://feed.example.com/")},
                     {"hello", std::string(R"({"subscribe":"posts"})")},
                     {"fields", strings{"/length"}}};
    socket.outputs["value"] = {control(1)};
    renderer r(1, rate, block);
    r.play(g.spec);
    r.run(block);
    const auto streams = r.engine.io().webInputs(portKind::webStream);
    REQUIRE(streams.size() == 1);
    CHECK(streams[0]->request().event == "edit");
    const auto sockets = r.engine.io().webInputs(portKind::webSocket);
    REQUIRE(sockets.size() == 1);
    CHECK(sockets[0]->request().hello == R"({"subscribe":"posts"})");
    CHECK(sockets[0]->request().fields == strings{"/length"});

    r.engine.io().deliverWeb(portKind::webSocket, "posts", frameOf({140.0f}));
    r.engine.io().deliverWeb(portKind::webStream, "edits", frameOf({1.0f}));
    r.engine.io().deliverWeb(portKind::webIn, "posts", frameOf({9.0f}));  // another kind
    r.run(block);
    CHECK(r.engine.pipes().control(1).value(0) == 140.0f);
    CHECK(r.engine.pipes().events(0).events().size() == 1);
}

namespace {

// metro → events 0 → webOut, with a constant 0.25 (an lfo of no depth)
// on 2-channel control 1.
graph reporter(double every) {
    graph g;
    g.pipe(control(1), 2).pipe(control(2)).pipe(events(0));
    auto& level = g.node("level", "lfo");
    level.params = {{"depth", 0.0}, {"offset", 0.25}};
    level.outputs["out"] = {control(1)};
    auto& m = g.node("metro", "metro");
    m.params["every"] = every;
    m.outputs["out"] = {events(0)};
    auto& out = g.node("report", "webOut");
    out.params = {{"url", std::string("http://localhost:8080/level")},
                  {"fields", strings{"left", "right"}},
                  {"method", std::string("put")}};
    out.inputs["in"] = {events(0)};
    out.inputs["value"] = {control(1)};
    out.outputs["dropped"] = {control(2)};
    return g;
}

}  // namespace

TEST_CASE("webOut queues a frame per event with its port's request (io.md 7.1 and 7.2)") {
    renderer r(1, rate, block);
    r.play(reporter(1.0).spec);  // a beat at 120 BPM: 24000 samples
    r.run(48000);
    std::vector<dataFrame> sent;
    r.engine.io().drainWebOut([&](const webPort& port, const dataFrame& f) {
        CHECK(port.owner() == "report");
        CHECK(port.request().method == "put");
        CHECK(port.request().fields == strings{"left", "right"});
        CHECK(port.request().timeout == 5.0);
        sent.push_back(f);
    });
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].count == 2);
    CHECK(sent[0].values[0] == 0.25f);
    CHECK(sent[0].values[1] == 0.25f);
    CHECK(r.engine.pipes().control(2).value() == 0.0f);
}

TEST_CASE("a full webOut queue drops requests and counts them on dropped (io.md 7.3)") {
    renderer r(1, rate, block);
    const auto drops = r.engine.io().drops();
    r.play(reporter(1.0 / 64).spec);  // 375 samples a beat: 128 events a second
    r.run(48000);
    // Nothing drained the 64-frame queue: the rest were dropped.
    const float dropped = r.engine.pipes().control(2).value();
    CHECK(dropped >= 60.0f);
    CHECK(r.engine.io().drops() == drops + static_cast<std::uint64_t>(dropped));
    std::size_t queued = 0;
    r.engine.io().drainWebOut([&](const webPort&, const dataFrame&) { ++queued; });
    CHECK(queued == webPort::outputFrames);

    // The count carries over a swap.
    r.play(reporter(1.0 / 64).spec, 2);
    r.run(block);
    CHECK(r.engine.pipes().control(2).value() >= dropped);
}

TEST_CASE("a missing field leaves its channel and sends no event when it is field 0 (io.md 8.2)") {
    renderer r(1, rate, block);
    r.play(weather().spec);
    r.run(block);
    r.engine.io().deliverWeb(portKind::webIn, "now", frameOf({21.5f, 3.0f}));
    r.run(block);
    dataFrame tempMissing = frameOf({0.0f, 4.0f});
    tempMissing.missing = 0b01;
    r.engine.io().deliverWeb(portKind::webIn, "now", tempMissing);
    r.run(block);
    CHECK(r.engine.pipes().control(1).value(0) == 21.5f);
    CHECK(r.engine.pipes().control(1).value(1) == 4.0f);
    CHECK(r.engine.pipes().events(0).events().empty());
}
