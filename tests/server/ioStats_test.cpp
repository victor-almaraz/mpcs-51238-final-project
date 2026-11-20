// Per-node I/O counts and errors reported at most once a second per node
// (io.md §10).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

#include <espresso/primitives/engine.h>
#include <espresso/protocol/messages.h>

#include "engineHost.h"
#include "ioStats.h"

using namespace server;
using std::chrono::milliseconds;
using strings = std::vector<std::string>;

namespace {

strings due(ioStats& s, std::chrono::steady_clock::time_point at) {
    strings out;
    for (const auto& e : s.due(at))
        out.push_back(e.node + ": " + e.message + " (" + std::to_string(e.count) + ")");
    return out;
}

}  // namespace

TEST_CASE("each node's counts add up separately") {
    ioStats s;
    s.add(strings{"a", "b"}, ioCount::frames, 3);
    s.add("a", ioCount::late);
    s.add("b", ioCount::missing, 2);
    CHECK(s.of("a")[ioCount::frames] == 3);
    CHECK(s.of("a")[ioCount::late] == 1);
    CHECK(s.of("b")[ioCount::missing] == 2);
    CHECK(s.of("b")[ioCount::late] == 0);
    CHECK(s.of("nobody")[ioCount::frames] == 0);
    CHECK(s.errors() == 0);
}

TEST_CASE("errors are reported at most once a second per node with their count (io.md 10.2)") {
    ioStats s;
    const std::chrono::steady_clock::time_point t0{};
    s.error("poll", "HTTP status 503");
    CHECK(due(s, t0) == strings{"poll: HTTP status 503 (1)"});
    // A dead endpoint polled every 0.1 s: one report a second, with the count.
    for (int i = 1; i <= 10; ++i) {
        s.error("poll", "HTTP status 50" + std::to_string(i % 10));
        CHECK(due(s, t0 + milliseconds(100 * i - 1)).empty());
    }
    CHECK(due(s, t0 + milliseconds(1000)) == strings{"poll: HTTP status 500 (10)"});
    CHECK(due(s, t0 + milliseconds(5000)).empty());  // nothing new
    // Nodes are limited separately.
    s.error(strings{"poll", "other"}, "timeout");
    CHECK(due(s, t0 + milliseconds(5000)) == strings{"other: timeout (1)", "poll: timeout (1)"});
    CHECK(s.errors() == 13);
    CHECK(s.of("poll")[ioCount::errors] == 12);
}

TEST_CASE("the engine host sends io.error and counts ioErrors (io.md 10.2 and 10.3)") {
    primitives::engine engine({.audio = {.sampleRate = 48000.0, .blockSize = 64}, .pipes = {}});
    engineHost host(engine);
    host.io().error("main.lisp/main/web-in@2:3#0", "web-in https://a.example/: HTTP status 503");
    const auto messages = host.poll();
    REQUIRE(messages.size() == 1);
    CHECK(messages[0].address == protocol::address::ioError);
    CHECK(messages[0].tags() == "ssi");
    CHECK(messages[0].string(0) == "main.lisp/main/web-in@2:3#0");
    CHECK(messages[0].int32(2) == 1);
    CHECK(host.counters().ioErrors == 1);
    CHECK(host.poll().empty());
}
