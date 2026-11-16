#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <proj/ir/ir.h>
#include <proj/metadata/version.h>
#include <proj/protocol/jthread.h>
#include <proj/protocol/osc.h>
#include <proj/protocol/serverSession.h>
#include <proj/protocol/udp.h>
#include <proj/tui/app.h>
#include <proj/tui/interpreterLink.h>

#include "support/tempDir.h"
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
    bool hasPlan(std::int32_t planId) const override { return plans.count(planId) != 0; }
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
          thread_([this](const proj::stop_token& stop) { loop(stop); }) {}

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

    void loop(const proj::stop_token& stop) {
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
    proj::jthread thread_;  // last: stops and joins before the members it uses go away
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
                .link = linkOptions{.server = {.serverPort = port, .timeoutMs = 100, .retries = 3},
                                    .clientVersion = "test",
                                    .connectOnStart = true}};
    }
};

bool consoleHas(app& a, std::string_view text) {
    a.collectReplies();
    for (const auto& e : a.console().entries()) {
        if (e.text.find(text) != std::string::npos) return true;
    }
    return false;
}

bool waitFor(app& a, std::string_view text) {
    for (int i = 0; i < 200; ++i) {
        if (consoleHas(a, text)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

TEST_CASE("play submits IR and reports the swap (protocol.md 4 and 6)", "[tui][server]") {
    testServer server{std::string(metadata::version)};
    project p;
    app a(p.options(server.port()));

    REQUIRE(waitFor(a, "server: connected to test-server"));
    CHECK(a.status().connected);

    a.runCommand("play");
    REQUIRE(waitFor(a, "server: plan 1 active"));
    CHECK(consoleHas(a, "server: plan 1 accepted"));
    CHECK(consoleHas(a, "play: compiled main.lisp: 0 node(s), 0 pipe(s)"));
    CHECK(ir::read(server.ir(1)).ok());
    CHECK(a.status().activePlan == 1);

    // Counters arrive ten times per second and update the status bar.
    for (int i = 0; i < 100 && a.status().xruns != 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        a.collectReplies();
    }
    CHECK(a.status().xruns == 3);

    a.runCommand("stop");
    REQUIRE(waitFor(a, "server: plan 1 retired"));
    CHECK(a.status().activePlan == 0);
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!started && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(started);
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
