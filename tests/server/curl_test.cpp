// The libcurl transport against a local server (io.md §14.4): one JSON
// poll, one event stream, one WebSocket with its hello, and one POST. It
// makes no external connection.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/primitives/io.h>

#include "curlTransport.h"
#include "ioStats.h"
#include "localServer.h"
#include "support/timing.h"
#include "webBridge.h"

using namespace server;
using primitives::dataFrame;
using primitives::portKind;
using strings = std::vector<std::string>;

namespace {

// The first value of the next frame a port receives, waiting for it.
std::optional<float> first(primitives::webPort& port) {
    dataFrame f;
    if (!support::waitUntil([&] { return port.queue.pop(f); })) return std::nullopt;
    return f.has(0) ? std::optional(f.values[0]) : std::nullopt;
}

}  // namespace

TEST_CASE("the libcurl transport polls streams holds a WebSocket and posts against a local server",
          "[network]") {
    const support::localServer server;
    primitives::ioHub hub;
    const auto now = hub.openWebIn(portKind::webIn, "now",
                                   {.url = server.url("/now"),
                                    .headers = {"Accept: application/json"},
                                    .fields = {"/main/temp"},
                                    .every = 60.0,
                                    .timeout = 5.0,
                                    .event = {},
                                    .hello = {},
                                    .method = {}});
    const auto edits = hub.openWebIn(portKind::webStream, "edits",
                                     {.url = server.url("/stream"),
                                      .headers = {},
                                      .fields = {"/n"},
                                      .every = 0.0,
                                      .timeout = 0.0,
                                      .event = "edit",
                                      .hello = {},
                                      .method = {}});
    const auto posts = hub.openWebIn(portKind::webSocket, "posts",
                                     {.url = server.url("/feed", "ws"),
                                      .headers = {},
                                      .fields = {"/length"},
                                      .every = 0.0,
                                      .timeout = 0.0,
                                      .event = {},
                                      .hello = R"({"subscribe":"posts"})",
                                      .method = {}});
    const auto report = hub.openWebOut("report", {.url = server.url("/level"),
                                                  .headers = {},
                                                  .fields = {"level"},
                                                  .every = 0.0,
                                                  .timeout = 5.0,
                                                  .event = {},
                                                  .hello = {},
                                                  .method = "post"});
    for (const auto& port : {now, edits, posts}) port->setActive(true);
    dataFrame level;
    level.count = 1;
    level.values[0] = 0.25f;
    REQUIRE(report->queue.push(level));

    ioStats stats;
    const webService web(
        hub, stats, {.clock = [] { return 0.0; }, .now = [] { return std::chrono::steady_clock::now(); }});
    CHECK(first(*now) == 21.5f);
    CHECK(first(*edits) == 7.0f);
    const auto socketValue = first(*posts);
    {
        std::string seen;
        for (const auto& r : server.requests()) seen += r + "; ";
        for (const auto& e : stats.due(std::chrono::steady_clock::now() + std::chrono::hours(1)))
            seen += "error: " + e.node + ": " + e.message + "; ";
        INFO(seen);
        CHECK(socketValue == 140.0f);
    }
    CHECK(support::waitUntil([&] { return server.posted() == strings{R"({"level":0.25})"}; }));
    CHECK(server.hellos() == strings{R"({"subscribe":"posts"})"});
    CHECK(stats.errors() == 0);
    CHECK(web.counts().errors == 0);
}
