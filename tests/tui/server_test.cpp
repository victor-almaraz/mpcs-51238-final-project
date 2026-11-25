#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
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
#include <espresso/protocol/messages.h>
#include <espresso/protocol/serverSession.h>
#include <espresso/protocol/udp.h>
#include <espresso/tui/app.h>
#include <espresso/workspace/consoleLog.h>
#include <espresso/workspace/interpreterLink.h>

#include "support/frame.h"
#include "support/tempDir.h"
#include "support/testServer.h"
#include "support/timing.h"
#include "support/unwrap.h"

namespace fs = std::filesystem;
using namespace tui;

namespace {

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
                .link = workspace::linkOptions{
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
    support::testServer server{std::string(metadata::version)};
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
    // The busiest of the server's threads (threads.md 8).
    CHECK(support::waitUntil([&] { return statusShows(a, "Plan 1 · Load 62% · Threads 4"); }));
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

TEST_CASE("a saved buffer and a failed save reach the console (samples.md 18.3)", "[tui][server]") {
    support::testServer server{std::string(metadata::version)};
    project p;
    app a(p.options(server.port()));
    REQUIRE(waitFor(a, "server: connected to test-server"));
    server.broadcast({.address = std::string(protocol::address::bufferSaved),
                      .args = {std::string("main.lisp/main/buffer@3:9#0"), std::string("takes/one.wav")}});
    CHECK(waitFor(a, "server: saved buffer main.lisp/main/buffer@3:9#0 to takes/one.wav"));
    server.broadcast(
        protocol::failure(0, protocol::errorCode::fileError,
                          "node save: cannot save buffer b to /x.wav: the file must be a path under "
                          "the project root"));
    CHECK(waitFor(a, "server: node save: cannot save buffer b to /x.wav"));
}

TEST_CASE("an I/O error reaches the console at its node's source (io.md 10.2)", "[tui][server]") {
    support::testServer server{std::string(metadata::version)};
    project p;
    p.write("main.lisp",
            "(defpiece main ()\n  (web-in :url \"https://a.example/\" :value (pipe :control)))\n");
    app a(p.options(server.port()));
    REQUIRE(waitFor(a, "server: connected to test-server"));
    a.runCommand("play");
    REQUIRE(waitFor(a, "server: plan 1 active"));
    const auto graph = support::unwrap(ir::read(server.ir(1)).graph);
    REQUIRE(graph.nodes.size() == 1);
    server.broadcast(
        {.address = std::string(protocol::address::ioError),
         .args = {graph.nodes[0].id, std::string("web-in https://a.example/: HTTP status 503"), 4}});
    REQUIRE(waitFor(a, "server: web-in https://a.example/: HTTP status 503 (4 errors)"));
    bool located = false;
    for (const auto& e : a.console().entries()) {
        if (!e.text.starts_with("server: web-in")) continue;
        CHECK(e.kind == workspace::consoleKind::error);
        const auto& loc = support::unwrap(e.location);
        CHECK(loc.path == "main.lisp");
        CHECK(loc.line == 2);
        located = true;
    }
    CHECK(located);
}

TEST_CASE("a metadata mismatch blocks IR submission (protocol.md 4.2)", "[tui][server]") {
    support::testServer server{"other-metadata"};
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
    workspace::serverStatus s;
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

TEST_CASE("the status bar shows the busiest thread's load, and the thread count past one (threads.md 8)",
          "[tui][server]") {
    workspace::serverStatus s;
    s.load = 0.45f;
    CHECK(s.loadText() == "Load —");  // not connected
    s.connected = true;
    CHECK(s.loadText() == "Load 45%");  // an older server reports no thread loads
    s.threadLoads = {0.45f};
    CHECK(s.loadText() == "Load 45%");
    s.threads = 3;
    s.threadLoads = {0.2f, 0.6f, 0.3f};
    CHECK(s.busiest() == 0.6f);
    CHECK(s.loadText() == "Load 60% · Threads 3");
    s.threadLoads = {1.5f, 0.1f, 0.1f};
    CHECK(s.loadText() == "Load 150% · Threads 3");  // over the period: deadlines missed
}

TEST_CASE("the editor keeps trying a server that starts after it and reports it once", "[tui][server]") {
    // Bound, so the port stays free for the server, but unanswered until
    // the server takes the socket over.
    auto socket = support::unwrap(protocol::udpSocket::bind(0));
    project p;
    auto options = p.options(socket.port());
    options.link.server.timeoutMs = 20 * support::slowdown;
    options.link.retryInterval = std::chrono::milliseconds(10);
    app a(options);
    REQUIRE(waitFor(a, "server: not reachable at 127.0.0.1:" + std::to_string(socket.port())));
    CHECK_FALSE(a.status().connected);

    const support::testServer server{std::string(metadata::version), std::move(socket)};
    REQUIRE(waitFor(a, "server: connected to test-server"));
    CHECK(a.status().connected);
    const auto lines = [&](std::string_view text) {
        return std::ranges::count_if(a.console().entries(),
                                     [&](const auto& e) { return e.text.find(text) != std::string::npos; });
    };
    CHECK(lines("not reachable") == 1);  // the retries are silent
    CHECK(lines("connected to test-server") == 1);
}

TEST_CASE("setWake waits for a wake in progress so its owner can go away", "[tui][server]") {
    workspace::interpreterLink link(
        workspace::linkOptions{.server = {.serverPort = 1, .timeoutMs = 5, .retries = 0},
                               .clientVersion = "test",
                               .connectOnStart = false});
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    link.setWake([&] {
        started = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        finished = true;
    });
    workspace::runRequest request;
    request.what = workspace::runRequest::kind::evalForm;
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

TEST_CASE("a plan refused at a buffer or a node points at its form (samples.md 4.6)", "[tui][server]") {
    support::testServer server{std::string(metadata::version)};
    project p;
    p.write("main.lisp", "(module main)\n(defpiece main ()\n  (let ((ring (buffer :file \"rain.wav\")))\n"
                         "    (sine-osc :out (pipe :audio))))\n");
    app a(p.options(server.port()));
    REQUIRE(waitFor(a, "server: connected to test-server"));
    for (const std::string member : {"buffers", "nodes"}) {
        // As the server words a build error: the ID of what failed first.
        server.refuseWith([member](const std::string& text) {
            const auto g = support::unwrap(ir::read(text).graph);
            const bool buffer = member == "buffers";
            const std::string id = buffer ? g.buffers.at(0).id : g.nodes.at(0).id;
            return std::optional(protocol::planError{.message = "cannot build it",
                                                     .subject = buffer ? protocol::failSubject::buffer
                                                                       : protocol::failSubject::node,
                                                     .id = id});
        });
        a.runCommand("play");
        REQUIRE(waitFor(a, ": cannot build it"));
        a.collectReplies();
        const auto& entries = a.console().entries();
        const auto refused = std::ranges::find_if(entries, [](const workspace::consoleEntry& e) {
            return e.text.find(": cannot build it") != std::string::npos;
        });
        REQUIRE(refused != entries.end());
        INFO(refused->text);
        const auto& at = support::unwrap(refused->location);
        CHECK(at.path == fs::path("main.lisp"));
        CHECK(at.line == (member == "buffers" ? 3 : 4));
        CHECK(at.column == (member == "buffers" ? 15 : 5));  // "  (let ((ring " is 14 columns
        a.runCommand("clearConsole");
        a.collectReplies();
    }
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
    auto server = std::make_unique<support::testServer>(std::string(metadata::version));
    const auto port = server->port();
    project p;
    app a(p.options(port));
    REQUIRE(waitFor(a, "server: connected to test-server"));
    server.reset();  // no more replies or counters
    CHECK(waitFor(a, "server: connection to 127.0.0.1:" + std::to_string(port) + " lost"));
    CHECK_FALSE(a.status().connected);
}
