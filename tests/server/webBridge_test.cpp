// The web bridge against a scripted transport and a fake clock (io.md §5,
// §6, §7, §14.1).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/primitives/io.h>

#include "ioStats.h"
#include "support/unwrap.h"
#include "webBridge.h"
#include "webData.h"
#include "webTransport.h"

using namespace server;
using primitives::dataFrame;
using primitives::portKind;
using primitives::webPort;
using primitives::webRequest;
using std::chrono::milliseconds;
using std::chrono::seconds;
using strings = std::vector<std::string>;

namespace {

// A transport whose transfers the test drives. What the test scripts is
// queued and happens in the next wait(), as a real transport's events do.
class fakeTransport final : public httpTransport {
public:
    struct transfer {
        httpRequest request;
        handlers events;
        bool socket = false;
        socketHandlers socketEvents;
        strings sent;  // text messages sent on a socket
        bool stopped = false;
    };

    id start(const httpRequest& request, handlers events) override {
        transfers_.push_back({.request = request,
                              .events = std::move(events),
                              .socket = false,
                              .socketEvents = {},
                              .sent = {},
                              .stopped = false});
        return transfers_.size() - 1;
    }
    id openSocket(const std::string& url, const std::vector<std::string>& headers,
                  socketHandlers events) override {
        transfers_.push_back(
            {.request = {.method = "GET", .url = url, .headers = headers, .body = {}, .timeout = {}},
             .events = {},
             .socket = true,
             .socketEvents = std::move(events),
             .sent = {},
             .stopped = false});
        return transfers_.size() - 1;
    }
    void sendText(id socket, std::string_view text) override {
        transfers_.at(socket).sent.emplace_back(text);
    }
    void stop(id t) override { transfers_.at(t).stopped = true; }
    void wait(milliseconds) override {
        auto due = std::exchange(pending_, {});
        for (auto& f : due) f();
    }

    // Scripting, for the next wait().
    void respond(id t, long status, const std::string& body) {
        later(t, [=](transfer& x) {
            x.events.data(status, body);
            x.events.done(status, "");
        });
    }
    void chunk(id t, const std::string& text, long status = 200) {
        later(t, [=](transfer& x) { x.events.data(status, text); });
    }
    void end(id t, long status, const std::string& error = {}) {
        later(t, [=](transfer& x) { x.events.done(status, error); });
    }
    void open(id t) {
        later(t, [](transfer& x) { x.socketEvents.opened(); });
    }
    void message(id t, const std::string& text, bool binary = false) {
        later(t, [=](transfer& x) { x.socketEvents.message(text, binary); });
    }
    void close(id t, const std::string& reason) {
        later(t, [=](transfer& x) { x.socketEvents.closed(reason); });
    }

    const transfer& at(id t) const { return transfers_.at(t); }
    std::size_t size() const noexcept { return transfers_.size(); }

private:
    void later(id t, std::function<void(transfer&)> f) {
        pending_.emplace_back([this, t, f = std::move(f)] {
            if (!transfers_.at(t).stopped) f(transfers_.at(t));
        });
    }

    std::vector<transfer> transfers_;
    std::vector<std::function<void()>> pending_;
};

// A bridge over a hub and a fake transport, on a fake clock.
struct rig {
    primitives::ioHub hub;
    fakeTransport transport;
    std::chrono::steady_clock::time_point t;
    ioStats stats;
    webBridge bridge{hub, transport, stats,
                     webOptions{.clock = [] { return 4.0; }, .now = [this] { return t; }}};

    // The errors due at the fake clock's time, as `node: message (count)`.
    strings reports() {
        strings out;
        for (const auto& e : stats.due(t))
            out.push_back(e.node + ": " + e.message + " (" + std::to_string(e.count) + ")");
        return out;
    }

    // An active web input of @p kind for node @p owner.
    std::shared_ptr<webPort> input(portKind kind, std::string owner, webRequest request) {
        auto port = hub.openWebIn(kind, std::move(owner), std::move(request));
        port->setActive(true);
        return port;
    }
    // Advances the clock by @p d, then runs a round.
    void after(milliseconds d) {
        t += d;
        bridge.step(milliseconds(0));
    }
    // Runs a round without moving the clock: the transport's queued events.
    void round() { bridge.step(milliseconds(0)); }
};

std::optional<dataFrame> next(webPort& port) {
    dataFrame f;
    if (!port.queue.pop(f)) return std::nullopt;
    return f;
}

std::vector<float> valuesOf(const std::optional<dataFrame>& f) {
    std::vector<float> out;
    if (!f) return out;
    for (std::size_t i = 0; i < f->count; ++i) out.push_back(f->has(i) ? f->values[i] : -999.0f);
    return out;
}

webRequest poll(std::string url, strings fields, double every = 10.0) {
    return {.url = std::move(url),
            .headers = {},
            .fields = std::move(fields),
            .every = every,
            .timeout = 5.0,
            .event = {},
            .hello = {},
            .method = {}};
}

}  // namespace

TEST_CASE("polls start when the plan is active and follow the interval from each start (io.md 5.2)") {
    rig r;
    auto idle = r.hub.openWebIn(portKind::webIn, "idle", poll("https://a.example/", {}));
    r.round();
    CHECK(r.transport.size() == 0);  // a plan that is not playing polls nothing
    idle->setActive(true);
    r.round();
    REQUIRE(r.transport.size() == 1);
    const auto& first = r.transport.at(0).request;
    CHECK(first.method == "GET");
    CHECK(first.url == "https://a.example/");
    CHECK(first.timeout == seconds(5));
    r.transport.respond(0, 200, "21.5");
    r.round();
    CHECK(valuesOf(next(*idle)) == std::vector<float>{21.5f});
    r.after(seconds(9));
    CHECK(r.transport.size() == 1);
    r.after(seconds(1));
    CHECK(r.transport.size() == 2);
    CHECK(r.bridge.counts().polls == 2);
}

TEST_CASE("nodes polling one URL with the same headers and interval share requests (io.md 5.3)") {
    rig r;
    auto temp = r.input(portKind::webIn, "temp", poll("https://w.example/now", {"/main/temp"}));
    auto wind = r.input(portKind::webIn, "wind", poll("https://w.example/now", {"/wind/speed", "/wind/deg"}));
    auto slow = r.input(portKind::webIn, "slow", poll("https://w.example/now", {"/main/temp"}, 60.0));
    r.round();
    REQUIRE(r.transport.size() == 2);  // one schedule every 10 s, one every 60 s
    r.transport.respond(0, 200, R"({"main": {"temp": 21.5}, "wind": {"speed": 3}})");
    r.round();
    CHECK(valuesOf(next(*temp)) == std::vector<float>{21.5f});
    const auto w = next(*wind);
    CHECK(valuesOf(w) == std::vector<float>{3.0f, -999.0f});  // a missing field
    CHECK(support::unwrap(w).time == 4.0);
    CHECK(!next(*slow));
    CHECK(r.bridge.counts().missing == 1);
    CHECK(r.bridge.counts().frames == 2);
}

TEST_CASE("a poll still running when the next is due is skipped and counted late (io.md 5.2)") {
    rig r;
    auto p = r.input(portKind::webIn, "p", poll("https://a.example/", {}, 1.0));
    r.round();
    r.after(seconds(1));
    r.after(seconds(1));
    CHECK(r.transport.size() == 1);
    CHECK(r.bridge.counts().late == 2);
    r.transport.respond(0, 200, "1");
    r.round();
    r.after(seconds(1));
    CHECK(r.transport.size() == 2);
}

TEST_CASE("a failed poll counts an error for each node and leaves the channels (io.md 5.4)") {
    rig r;
    auto p = r.input(portKind::webIn, "p", poll("https://a.example/", {}, 1.0));
    r.round();
    // Each response is read in the round after it, before the next poll.
    const auto answer = [&](std::size_t request, const std::function<void()>& script) {
        REQUIRE(r.transport.size() == request + 1);
        script();
        r.round();
        r.after(seconds(1));
    };
    answer(0, [&] { r.transport.respond(0, 503, "{}"); });
    answer(1, [&] { r.transport.respond(1, 200, "<html>"); });
    answer(2, [&] { r.transport.end(2, 0, "Timeout was reached"); });
    answer(3, [&] { r.transport.respond(3, 200, "[" + std::string(maxDocumentBytes, ' ') + "]"); });
    CHECK(!next(*p));
    CHECK(r.bridge.counts().errors == 4);
    CHECK(r.stats.of("p")[ioCount::errors] == 4);
    // One report carries the latest message and how many errors occurred.
    CHECK(r.reports() == strings{"p: web-in https://a.example/: the response is larger than 1 MiB (4)"});
    answer(4, [&] { r.transport.respond(4, 200, "7"); });
    answer(5, [&] { r.transport.respond(5, 500, ""); });
    CHECK(valuesOf(next(*p)) == std::vector<float>{7.0f});
    CHECK(r.reports() == strings{"p: web-in https://a.example/: HTTP status 500 (1)"});
}

TEST_CASE("a schedule whose nodes are gone stops its request") {
    rig r;
    auto p = r.input(portKind::webIn, "p", poll("https://a.example/", {}));
    r.round();
    p->setActive(false);
    r.round();
    CHECK(r.transport.at(0).stopped);
    r.after(seconds(20));
    CHECK(r.transport.size() == 1);
}

namespace {

webRequest streamOf(std::string event, strings fields) {
    return {.url = "https://s.example/stream",
            .headers = {"Authorization: Bearer k"},
            .fields = std::move(fields),
            .every = 0.0,
            .timeout = 0.0,
            .event = std::move(event),
            .hello = {},
            .method = {}};
}

}  // namespace

TEST_CASE("a stream delivers each message to the nodes of its event (io.md 6.1 and 6.2)") {
    rig r;
    auto edits = r.input(portKind::webStream, "edits", streamOf("edit", {"/size"}));
    auto all = r.input(portKind::webStream, "all", streamOf("", {"/n"}));
    r.round();
    REQUIRE(r.transport.size() == 1);  // one connection for one URL and headers
    const auto& request = r.transport.at(0).request;
    CHECK(request.timeout == milliseconds(0));
    CHECK(request.headers == strings{"Authorization: Bearer k", "Accept: text/event-stream"});
    r.transport.chunk(0, "event: edit\ndata: {\"size\": 120, \"n\": 1}\n\ndata: {\"n\": 2}\n\n");
    r.transport.chunk(0, "data: not json\n\n");
    r.round();
    CHECK(valuesOf(next(*edits)) == std::vector<float>{120.0f});
    CHECK(!next(*edits));
    CHECK(valuesOf(next(*all)) == std::vector<float>{1.0f});
    CHECK(valuesOf(next(*all)) == std::vector<float>{2.0f});
    CHECK(r.bridge.counts().malformed == 1);
}

TEST_CASE(
    "a stream reconnects after 3 s doubling while failures repeat and sends its last event ID (io.md 6.3)") {
    rig r;
    auto s = r.input(portKind::webStream, "s", streamOf("", {}));
    r.round();
    r.transport.chunk(0, "id: 7\ndata: 1\n\n");
    r.transport.end(0, 200);  // the stream ended: no error
    r.round();
    CHECK(r.bridge.counts().errors == 0);
    r.after(milliseconds(2999));
    CHECK(r.transport.size() == 1);
    r.after(milliseconds(1));
    REQUIRE(r.transport.size() == 2);
    CHECK(r.transport.at(1).request.headers.back() == "Last-Event-ID: 7");
    r.transport.end(1, 0, "Could not connect");
    r.round();
    r.after(milliseconds(5999));
    CHECK(r.transport.size() == 2);
    r.after(milliseconds(1));  // 6 s: doubled
    REQUIRE(r.transport.size() == 3);
    r.transport.end(2, 502);
    r.round();
    CHECK(r.bridge.counts().errors == 2);
    CHECK(r.reports() ==
          strings{"s: web-stream https://s.example/stream: HTTP status 502; reconnecting (2)"});
    // A retry time from the server replaces the 3 s.
    r.after(seconds(12));
    REQUIRE(r.transport.size() == 4);
    r.transport.chunk(3, "retry: 500\n\n");
    r.transport.end(3, 200);
    r.round();
    r.after(milliseconds(500));
    CHECK(r.transport.size() == 5);
}

namespace {

webRequest socketOf(std::string hello, strings fields) {
    return {.url = "wss://f.example/feed",
            .headers = {},
            .fields = std::move(fields),
            .every = 0.0,
            .timeout = 0.0,
            .event = {},
            .hello = std::move(hello),
            .method = {}};
}

}  // namespace

TEST_CASE(
    "a socket sends its hello after each connection opens and delivers text messages (io.md 6.4 and 6.5)") {
    rig r;
    auto posts = r.input(portKind::webSocket, "posts", socketOf(R"({"subscribe":"posts"})", {"/length"}));
    r.round();
    REQUIRE(r.transport.size() == 1);
    CHECK(r.transport.at(0).socket);
    CHECK(r.transport.at(0).request.url == "wss://f.example/feed");
    r.transport.open(0);
    r.transport.message(0, R"({"length": 140})");
    r.transport.message(0, "\x01\x02", true);
    r.transport.message(0, "nope");
    r.round();
    CHECK(r.transport.at(0).sent == strings{R"({"subscribe":"posts"})"});
    CHECK(valuesOf(next(*posts)) == std::vector<float>{140.0f});
    CHECK(r.bridge.counts().skipped == 1);
    CHECK(r.bridge.counts().malformed == 1);

    // A close reconnects after 3 s and says hello again (io.md 6.6).
    r.transport.close(0, "server closed");
    r.round();
    CHECK(r.bridge.counts().errors == 0);  // it had opened
    r.after(seconds(3));
    REQUIRE(r.transport.size() == 2);
    r.transport.close(1, "Could not resolve host");  // never opened: an error
    r.round();
    CHECK(r.bridge.counts().errors == 1);
    r.after(seconds(3));
    CHECK(r.transport.size() == 2);
    r.after(seconds(3));  // doubled: 6 s
    REQUIRE(r.transport.size() == 3);
    r.transport.open(2);
    r.round();
    CHECK(r.transport.at(2).sent == strings{R"({"subscribe":"posts"})"});
}

TEST_CASE("webOut bodies are JSON objects of fields and values and get puts them in the query (io.md 7.2)") {
    webRequest post{.url = "http://localhost:8080/level",
                    .headers = {"X-Key: k"},
                    .fields = {"temp", "level"},
                    .every = 0.0,
                    .timeout = 2.0,
                    .event = {},
                    .hello = {},
                    .method = "post"};
    dataFrame f;
    f.count = 2;
    f.values = {21.5f, 0.1f};
    auto r = outRequest(post, f);
    CHECK(r.method == "POST");
    CHECK(r.url == "http://localhost:8080/level");
    CHECK(r.body == R"({"temp":21.5,"level":0.1})");
    CHECK(r.headers == strings{"X-Key: k", "Content-Type: application/json"});
    CHECK(r.timeout == seconds(2));

    post.method = "put";
    post.fields = {"a \"b\""};
    CHECK(outRequest(post, f).method == "PUT");
    CHECK(outRequest(post, f).body ==
          R"({"a \"b\"":21.5})");  // escaped; the value past the fields is left out

    webRequest get = post;
    get.method = "get";
    get.fields = {"temp", "a b&c"};
    get.url = "http://localhost:8080/r?site=1";
    r = outRequest(get, f);
    CHECK(r.method == "GET");
    CHECK(r.url == "http://localhost:8080/r?site=1&temp=21.5&a%20b%26c=0.1");
    CHECK(r.body.empty());
    CHECK(r.headers == strings{"X-Key: k"});

    // An event's number, without fields, is the value.
    webRequest plain = post;
    plain.fields = {};
    dataFrame one;
    one.count = 1;
    one.values[0] = -3.0f;
    CHECK(outRequest(plain, one).body == R"({"value":-3})");
}

TEST_CASE("webOut sends a request per queued frame and counts a failed one without retrying (io.md 7.4)") {
    rig r;
    auto out = r.hub.openWebOut("report", {.url = "http://localhost:8080/",
                                           .headers = {},
                                           .fields = {"x"},
                                           .every = 0.0,
                                           .timeout = 5.0,
                                           .event = {},
                                           .hello = {},
                                           .method = "post"});
    dataFrame f;
    f.count = 1;
    f.values[0] = 1.0f;
    REQUIRE(out->queue.push(f));
    REQUIRE(out->queue.push(f));
    r.round();
    REQUIRE(r.transport.size() == 2);
    CHECK(r.transport.at(1).request.body == R"({"x":1})");
    r.transport.respond(0, 201, "ok");
    r.transport.end(1, 0, "Connection refused");
    r.round();
    r.round();
    CHECK(r.transport.size() == 2);
    CHECK(r.bridge.counts().sent == 2);
    CHECK(r.bridge.counts().errors == 1);
    CHECK(r.reports() == strings{"report: web-out http://localhost:8080/: Connection refused (1)"});
    CHECK(r.stats.of("report")[ioCount::frames] == 2);
}
