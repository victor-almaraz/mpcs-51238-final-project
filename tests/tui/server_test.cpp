#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <espresso/ir/ir.h>
#include <espresso/metadata/version.h>
#include <espresso/protocol/jthread.h>
#include <espresso/protocol/osc.h>
#include <espresso/protocol/serverSession.h>
#include <espresso/protocol/udp.h>
#include <espresso/tui/app.h>
#include <espresso/tui/interpreterLink.h>

#include "support/frame.h"
#include "support/tempDir.h"
#include "support/timing.h"
#include "support/unwrap.h"

namespace fs = std::filesystem;
using namespace tui;

namespace {

// Activates plans as soon as they are played.
class immediateEngine : public protocol::engine {
public:
    std::optional<std::string> loadPlan(std::int32_t planId, std::string ir) override {
        plans[planId] = std::move(ir);
        return std::nullopt;
    }
    bool hasPlan(std::int32_t planId) const override { return plans.contains(planId); }
    void play(std::int32_t planId, float, std::string_view) override { pending.push_back(planId); }
    void stop(float) override {
        if (active != 0) retired.push_back(active);
        active = 0;
    }
    void setControl(std::int32_t, float) override {}
    float control(std::int32_t) const override { return 0.0f; }
    protocol::serverCounters counters() const override {
        protocol::serverCounters c;
        c.xruns = 3;
        c.activePlan = active;
        c.tempo = 96.0f;
        c.beat = 9.5;  // bar 3, beat 2 in 4/4: beats 8 to 11 are bar 3
        c.meter = 4;
        return c;
    }
    void quit() override {}

    std::map<std::int32_t, std::string> plans;
    std::vector<std::int32_t> pending;
    std::vector<std::int32_t> retired;
    std::int32_t active = 0;
};

// A server session on a loopback socket, served from its own thread.
class testServer {
public:
    explicit testServer(std::string metadataVersion)
        : session_({.serverVersion = "test-server",
                    .metadataVersion = std::move(metadataVersion),
                    .sampleRate = 48000,
                    .blockSize = 256},
                   engine_),
          socket_(support::unwrap(protocol::udpSocket::bind(0))),
          thread_([this](const espresso::stop_token& stop) { loop(stop); }) {}

    std::uint16_t port() const { return socket_.port(); }

    std::string ir(std::int32_t plan) {
        std::scoped_lock lock(mutex_);
        const auto it = engine_.plans.find(plan);
        return it == engine_.plans.end() ? std::string() : it->second;
    }

private:
    void send(const std::vector<protocol::outgoing>& out) {
        for (const auto& o : out) socket_.send(protocol::encode(o.msg), o.to);
    }

    void loop(const espresso::stop_token& stop) {
        auto nextCounters = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            auto d = socket_.receive(5);
            std::scoped_lock lock(mutex_);
            if (d) send(session_.handleDatagram(d->bytes, d->from));
            for (auto plan : std::exchange(engine_.pending, {})) {
                engine_.active = plan;
                send(session_.broadcast({.address = "/plan.active", .args = {plan}}));
            }
            for (auto plan : std::exchange(engine_.retired, {})) {
                send(session_.broadcast({.address = "/plan.retired", .args = {plan}}));
            }
            if (std::chrono::steady_clock::now() >= nextCounters) {
                send(session_.countersMessages());
                nextCounters += std::chrono::milliseconds(100);
            }
        }
    }

    immediateEngine engine_;
    protocol::serverSession session_;
    protocol::udpSocket socket_;
    std::mutex mutex_;
    espresso::jthread thread_;  // last: stops and joins before the members it uses go away
};

struct project : support::tempDir {
    const fs::path root = path();
    project() {
        write("main.lisp", "(module main)\n(defpiece main () nil)\n");
        write(".settings.lisp", "(settings)");
    }
    appOptions options(std::uint16_t port) const {
        return {.projectRoot = root,
                .files = {},
                .settingsFile = root / ".settings.lisp",
                .persistSettings = false,
                .link = linkOptions{
                    .server = {.serverPort = port, .timeoutMs = support::replyTimeoutMs, .retries = 3},
                    .clientVersion = "test",
                    .connectOnStart = true}};
    }
};

bool consoleHas(app& a, std::string_view text) {
    a.collectReplies();
    return std::ranges::any_of(a.console().entries(),
                               [&](const auto& e) { return e.text.find(text) != std::string::npos; });
}

bool waitFor(app& a, std::string_view text) {
    return support::waitUntil([&] { return consoleHas(a, text); });
}

// Whether the status bar, on a frame wide enough for all of it, shows
// @p text. The first frame lays out the window the second one fits to.
bool statusShows(app& a, const std::string& text) {
    a.collectReplies();
    const support::frame layout(a, 160);
    return support::frame(a, 160).contains(text);
}

// Whether the header bar's Stop is drawn as a control that cannot be
// used: faint, in the default theme (tui.md 4.3).
bool stopDimmed(app& a) {
    const support::frame f(a);
    const auto [x, y] = f.find("■");  // the header bar is the top row
    return f.screen.CellAt(x, y).dim;
}

} // namespace

TEST_CASE("play submits IR and reports the swap (protocol.md 4 and 6)", "[tui][server]") {
    testServer server{std::string(metadata::version)};
    project p;
    app a(p.options(server.port()));

    REQUIRE(waitFor(a, "server: connected to test-server"));
    CHECK(a.status().connected);
    CHECK(stopDimmed(a));  // nothing plays yet
    // Tempo and beat arrive with the counters; no beat while nothing plays.
    CHECK(support::waitUntil([&] { return statusShows(a, "96 BPM · Beat — · Plan —"); }));

    a.runCommand("play");
    REQUIRE(waitFor(a, "server: plan 1 active"));
    CHECK(support::waitUntil([&] { return statusShows(a, "96 BPM · Beat 3.2 · Plan 1"); }));
    CHECK_FALSE(stopDimmed(a));
    CHECK(consoleHas(a, "server: plan 1 accepted"));
    CHECK(consoleHas(a, "play: compiled main.lisp: 0 node(s), 0 pipe(s)"));
    CHECK(ir::read(server.ir(1)).ok());
    CHECK(a.status().activePlan == 1);

    // Counters arrive ten times per second and update the status bar.
    CHECK(support::waitUntil([&] {
        a.collectReplies();
        return a.status().xruns == 3;
    }));

    a.runCommand("stop");
    REQUIRE(waitFor(a, "server: plan 1 retired"));
    CHECK(a.status().activePlan == 0);
    CHECK(stopDimmed(a));
}

TEST_CASE("a metadata mismatch blocks IR submission (protocol.md 4.2)", "[tui][server]") {
    testServer server{"other-metadata"};
    project p;
    app a(p.options(server.port()));
    REQUIRE(waitFor(a, "metadata version mismatch"));
    CHECK_FALSE(a.status().connected);
    a.runCommand("play");
    a.link().waitIdle();
    a.collectReplies();
    CHECK(server.ir(1).empty());
}

TEST_CASE("a missing server is reported and not fatal", "[tui][server]") {
    const auto silent = support::unwrap(protocol::udpSocket::bind(0));  // nobody answers on this port
    project p;
    auto options = p.options(silent.port());
    options.link.server.timeoutMs = 10;
    app a(options);
    REQUIRE(waitFor(a, "server: not reachable at 127.0.0.1:" + std::to_string(silent.port())));
    CHECK_FALSE(a.status().connected);
    CHECK(statusShows(a, "Not connected   — BPM · Beat — · Missed 0"));
}

TEST_CASE("the status bar shows the tempo to one decimal and the beat as bar.beat (tui.md 3)",
          "[tui][server]") {
    serverStatus s;
    s.bpm = 133.333;
    s.beat = 13.99;
    s.meter = 3;
    CHECK(s.tempoText() == "— BPM");  // not connected
    CHECK(s.beatText() == "—");
    s.connected = true;
    CHECK(s.tempoText() == "133.3 BPM");
    CHECK(s.beatText() == "—");  // no plan active
    s.activePlan = 2;
    CHECK(s.beatText() == "5.2");  // beat 13 of 3-beat bars: bar 5 holds beats 12 to 14
    s.beat = 0.0;
    CHECK(s.beatText() == "1.1");
    s.meter = 0;  // a meter below 1 counts as 1
    s.beat = 4.5;
    CHECK(s.beatText() == "5.1");
    s.bpm = 120.0;
    CHECK(s.tempoText() == "120 BPM");
    s.bpm.reset();
    s.beat.reset();
    CHECK(s.tempoText() == "— BPM");  // an older server sends neither
    CHECK(s.beatText() == "—");
}

TEST_CASE("setWake waits for a wake in progress so its owner can go away", "[tui][server]") {
    interpreterLink link(linkOptions{.server = {.serverPort = 1, .timeoutMs = 5, .retries = 0},
                                     .clientVersion = "test",
                                     .connectOnStart = false});
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    link.setWake([&] {
        started = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        finished = true;
    });
    runRequest request;
    request.what = runRequest::kind::evalForm;
    request.form = "(+ 1 2)";
    link.submit(std::move(request));
    REQUIRE(support::waitUntil([&] { return started.load(); }));
    link.setWake({});  // the app does this before its screen is destroyed
    CHECK(finished);
}

TEST_CASE("play compiles imports from the project and locates their errors", "[tui][server]") {
    const auto silent = support::unwrap(protocol::udpSocket::bind(0));  // compiles without a server
    project p;
    p.write("voices.lisp", "(define (tone out)\n  (sine-osc :freq 220hz :out out))\n");
    p.write("main.lisp", "(import \"voices\")\n(defpiece main () (tone (pipe :audio)))\n");
    auto options = p.options(silent.port());
    options.link.server.timeoutMs = 10;
    app a(options);
    a.runCommand("play");
    REQUIRE(waitFor(a, "play: compiled main.lisp: 1 node(s), 1 pipe(s)"));
    p.write("voices.lisp", "(define (tone out)\n  (sine-osc :freq 1s :out out))\n");
    a.runCommand("play");
    REQUIRE(waitFor(a, "voices.lisp:2:19: parameter :freq expects a frequency"));
    CHECK(consoleHas(a, "  called from main.lisp:2:19"));
}

TEST_CASE("a standard library line in the console names its file without a location to open",
          "[tui][server]") {
    const auto silent = support::unwrap(protocol::udpSocket::bind(0));
    project p;
    // voice-split needs an event pipe; the error is inside std/voices.lisp.
    p.write("main.lisp", "(import \"std/voices\")\n(defpiece main () (voices 2 (pipe :audio) (fn (n) 1)))\n");
    auto options = p.options(silent.port());
    options.link.server.timeoutMs = 10;
    app a(options);
    a.runCommand("play");
    REQUIRE(waitFor(a, "did not compile"));
    bool library = false;
    bool project = false;
    for (const auto& e : a.console().entries()) {
        if (e.text.starts_with("std/voices.lisp:")) library = !e.location.has_value();
        if (e.text.starts_with("  called from main.lisp:")) project = e.location.has_value();
    }
    CHECK(library);
    CHECK(project);
}

TEST_CASE("a server that goes silent is reported lost", "[tui][server]") {
    auto server = std::make_unique<testServer>(std::string(metadata::version));
    const auto port = server->port();
    project p;
    app a(p.options(port));
    REQUIRE(waitFor(a, "server: connected to test-server"));
    server.reset();  // no more replies or counters
    CHECK(waitFor(a, "server: connection to 127.0.0.1:" + std::to_string(port) + " lost"));
    CHECK_FALSE(a.status().connected);
}
