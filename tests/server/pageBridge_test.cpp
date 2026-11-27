// The page bridge against a socket client on 127.0.0.1 (io.md §12, §14.1):
// files from ui/, messages posted in order and streamed back, the
// refusals of §12.5 and §12.7, the connection limit, and the listener
// following the active plan.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>
#include <espresso/page/render.h>
#include <espresso/primitives/engine.h>
#include <espresso/protocol/jthread.h>
#include <espresso/protocol/osc.h>

#include "engineHost.h"
#include "pageBridge.h"
#include "pageHelper.h"
#include "pageMessages.h"
#include "pagePlan.h"
#include "support/files.h"
#include "support/graph.h"
#include "support/sockets.h"
#include "support/tempDir.h"
#include "support/timing.h"
#include "support/unwrap.h"
#include "webSocket.h"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using namespace server;

namespace {

// A connection to the page port, written and read as raw HTTP.
using client = support::httpClient;

std::string get(std::string_view path, std::uint16_t port, std::string_view host = {}) {
    const std::string h = host.empty() ? "127.0.0.1:" + std::to_string(port) : std::string(host);
    return "GET " + std::string(path) + " HTTP/1.1\r\nHost: " + h + "\r\n\r\n";
}

std::string post(std::string_view body, std::uint16_t port, std::string_view origin = "own") {
    const std::string host = "localhost:" + std::to_string(port);
    std::string out = "POST /osc HTTP/1.1\r\nHost: " + host + "\r\n";
    if (origin == "own") {
        out += "Origin: http://" + host + "\r\n";
    } else if (!origin.empty()) {
        out += "Origin: " + std::string(origin) + "\r\n";
    }
    return out + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
}

// A project with ui/index.html, ui/app.js, ui/sub/style.css, a secret
// beside ui/, and a symbolic link in ui/ that leaves it.
struct site : support::tempDir {
    site() {
        std::filesystem::create_directories(path() / "ui" / "sub");
        std::ofstream(path() / "ui" / "index.html") << "<h1>keys</h1>";
        std::ofstream(path() / "ui" / "app.js") << "espresso.send('/a', 1);";
        std::ofstream(path() / "ui" / "sub" / "style.css") << "body {}";
        std::ofstream(path() / "secret.txt") << "token";
        std::filesystem::create_symlink(path() / "secret.txt", path() / "ui" / "link.txt");
    }
};

std::unique_ptr<pageBridge> listening(const site& s, std::string helper = {}, std::string controls = {}) {
    auto b = std::make_unique<pageBridge>(pageOptions{
        .root = s.path(), .helper = std::move(helper), .controls = std::move(controls), .report = {}});
    b->listen(0);
    REQUIRE(b->port() != 0);
    return b;
}

protocol::message decoded(const oscBytes& b) { return support::unwrap(protocol::decode(b)); }

}  // namespace

TEST_CASE("the page port serves the files of ui/ and / as index.html (io.md 12.2)") {
    const site s;
    const auto bridge = listening(s, "/* helper */");
    client c(bridge->port());
    REQUIRE(c.connected());
    // One connection, kept alive across requests.
    c.write(get("/", bridge->port()) + get("/app.js", bridge->port()) +
            get("/sub/style.css", bridge->port()) + get("/espresso.js", bridge->port()));
    auto r = c.response();
    CHECK_THAT(r, StartsWith("HTTP/1.1 200 OK\r\n"));
    CHECK_THAT(r, ContainsSubstring("Content-Type: text/html; charset=utf-8"));
    CHECK_THAT(r, ContainsSubstring("\r\n\r\n<h1>keys</h1>"));
    CHECK_THAT(c.response(), ContainsSubstring("text/javascript"));
    CHECK_THAT(c.response(), ContainsSubstring("text/css"));
    r = c.response();
    CHECK_THAT(r, ContainsSubstring("/* helper */"));
    CHECK_THAT(r, !ContainsSubstring("Access-Control"));  // no CORS headers (io.md 12.7)
}

TEST_CASE("without ui/index.html, / is the page drawn from the layout (dsl-ui.md 7.1)") {
    const support::tempDir empty;
    auto bridge = std::make_unique<pageBridge>(
        pageOptions{.root = empty.path(), .helper = {}, .controls = {}, .report = {}});
    bridge->listen(0);
    REQUIRE(bridge->port() != 0);
    client c(bridge->port());
    c.write(get("/", bridge->port()));
    CHECK_THAT(c.response(), StartsWith("HTTP/1.1 404"));
    bridge->serve(std::make_shared<const std::string>("<h1>drawn</h1>"));
    c.write(get("/", bridge->port()));
    const auto r = c.response();
    CHECK_THAT(r, StartsWith("HTTP/1.1 200 OK\r\n"));
    CHECK_THAT(r, ContainsSubstring("Content-Type: text/html; charset=utf-8"));
    CHECK_THAT(r, ContainsSubstring("\r\n\r\n<h1>drawn</h1>"));
    // A project's own page takes its place.
    const site s;
    const auto own = listening(s);
    own->serve(std::make_shared<const std::string>("<h1>drawn</h1>"));
    client d(own->port());
    d.write(get("/", own->port()));
    CHECK_THAT(d.response(), ContainsSubstring("<h1>keys</h1>"));
}

TEST_CASE("a project's style and script, which a drawn page loads, are its files, else empty (dsl-ui.md 8)") {
    const support::tempDir root;
    std::filesystem::create_directories(root.path() / "ui");
    std::ofstream(root.path() / "ui" / "style.css") << "body { color: red; }";
    auto bridge = std::make_unique<pageBridge>(
        pageOptions{.root = root.path(), .helper = {}, .controls = {}, .report = {}});
    bridge->listen(0);
    REQUIRE(bridge->port() != 0);
    client c(bridge->port());
    c.write(get("/style.css", bridge->port()) + get("/script.js", bridge->port()));
    CHECK_THAT(c.response(), ContainsSubstring("\r\n\r\nbody { color: red; }"));
    const auto script = c.response();
    CHECK_THAT(script, StartsWith("HTTP/1.1 200 OK\r\n"));
    CHECK_THAT(script, ContainsSubstring("Content-Type: text/javascript"));
    CHECK_THAT(script, ContainsSubstring("Content-Length: 0\r\n"));
}

TEST_CASE("a page unlike the one served asks open pages to load it again (dsl-ui.md 7.1)") {
    const support::tempDir empty;
    auto bridge = std::make_unique<pageBridge>(
        pageOptions{.root = empty.path(), .helper = {}, .controls = {}, .report = {}});
    bridge->listen(0);
    REQUIRE(bridge->port() != 0);
    bridge->serve(std::make_shared<const std::string>("<h1>one</h1>"));
    client stream(bridge->port());
    stream.write(get("/osc", bridge->port()));
    stream.readUntil([](const std::string& b) { return b.find("\r\n\r\n") != std::string::npos; });
    // Once the stream is registered, a ping reaches it.
    REQUIRE(support::waitUntil([&] {
        bridge->send(protocol::encode({.address = "/ping", .args = {}}));
        return stream.readUntil([](const std::string& b) { return b.find("/ping") != std::string::npos; })
                   .find("/ping") != std::string::npos;
    }));
    // The same text again asks nothing.
    bridge->serve(std::make_shared<const std::string>("<h1>one</h1>"));
    bridge->serve(std::make_shared<const std::string>("<h1>two</h1>"));
    bridge->send(protocol::encode({.address = "/after", .args = {}}));
    const auto seen =
        stream.readUntil([](const std::string& b) { return b.find("/after") != std::string::npos; });
    CHECK_THAT(seen, ContainsSubstring("data: [\"/espresso/reload\"]\n\n"));
    CHECK(seen.find("/espresso/reload") == seen.rfind("/espresso/reload"));
    // A stop serves no page; a run of another layout after it still asks.
    bridge->serve(nullptr);
    bridge->serve(std::make_shared<const std::string>("<h1>three</h1>"));
    bridge->send(protocol::encode({.address = "/later", .args = {}}));
    const auto later =
        stream.readUntil([](const std::string& b) { return b.find("/later") != std::string::npos; });
    CHECK(later.find("/espresso/reload", seen.size()) != std::string::npos);
}

TEST_CASE("a project's own page is never asked to reload, and an empty ui/index.html is no page") {
    const support::tempDir root;
    std::filesystem::create_directories(root.path() / "ui");
    {
        const std::ofstream empty(root.path() / "ui" / "index.html");  // as a failed --page redirect leaves it
    }
    auto bridge = std::make_unique<pageBridge>(
        pageOptions{.root = root.path(), .helper = {}, .controls = {}, .report = {}});
    bridge->listen(0);
    REQUIRE(bridge->port() != 0);
    bridge->serve(std::make_shared<const std::string>("<h1>drawn</h1>"));
    client c(bridge->port());
    c.write(get("/", bridge->port()) + get("/index.html", bridge->port()));
    CHECK_THAT(c.response(), ContainsSubstring("\r\n\r\n<h1>drawn</h1>"));
    CHECK_THAT(c.response(), ContainsSubstring("\r\n\r\n<h1>drawn</h1>"));
    // With a page of its own, a new layout sends no reload.
    std::ofstream(root.path() / "ui" / "index.html") << "<h1>mine</h1>";
    client stream(bridge->port());
    stream.write(get("/osc", bridge->port()));
    stream.readUntil([](const std::string& b) { return b.find("\r\n\r\n") != std::string::npos; });
    REQUIRE(support::waitUntil([&] {
        bridge->send(protocol::encode({.address = "/ping", .args = {}}));
        return stream.readUntil([](const std::string& b) { return b.find("/ping") != std::string::npos; })
                   .find("/ping") != std::string::npos;
    }));
    bridge->serve(std::make_shared<const std::string>("<h1>changed</h1>"));
    bridge->send(protocol::encode({.address = "/after", .args = {}}));
    const auto seen =
        stream.readUntil([](const std::string& b) { return b.find("/after") != std::string::npos; });
    CHECK(seen.find("/espresso/reload") == std::string::npos);
}

TEST_CASE("the page port refuses files outside ui/ symbolic links that leave it and directories with 404") {
    const site s;
    const auto bridge = listening(s);
    for (const std::string path : {"/missing.html", "/../secret.txt", "/%2e%2e/secret.txt",
                                   "/sub/../../secret.txt", "/link.txt", "/sub", "/%zz", "/espresso.js"}) {
        INFO(path);
        client c(bridge->port());
        c.write(get(path, bridge->port()));
        CHECK_THAT(c.response(), StartsWith("HTTP/1.1 404 "));
    }
}

TEST_CASE("the page port answers only Hosts of 127.0.0.1 and localhost at its port (io.md 12.7)") {
    const site s;
    const auto bridge = listening(s);
    const auto port = bridge->port();
    for (const std::string& host :
         std::vector<std::string>{"evil.example:" + std::to_string(port), "127.0.0.1:1", "127.0.0.1"}) {
        INFO(host);
        client c(port);
        c.write(get("/", port, host));
        CHECK_THAT(c.response(), StartsWith("HTTP/1.1 403 "));
    }
    client none(port);
    none.write("GET / HTTP/1.1\r\n\r\n");
    CHECK_THAT(none.response(), StartsWith("HTTP/1.1 403 "));
    client ok(port);
    ok.write(get("/", port, "localhost:" + std::to_string(port)));
    CHECK_THAT(ok.response(), StartsWith("HTTP/1.1 200 "));
}

TEST_CASE("posted messages arrive in order and a refused batch delivers none (io.md 12.5)") {
    const site s;
    const auto bridge = listening(s);
    const auto port = bridge->port();
    client c(port);
    c.write(post(R"([["/cutoff", 0.4], ["/note", 60, 100]])", port) + post(R"([["/gate", true]])", port));
    CHECK_THAT(c.response(), StartsWith("HTTP/1.1 204 "));
    CHECK_THAT(c.response(), StartsWith("HTTP/1.1 204 "));
    const auto got = bridge->takeMessages();
    REQUIRE(got.size() == 3);
    CHECK(decoded(got[0]).address == "/cutoff");
    CHECK(decoded(got[1]) == protocol::message{.address = "/note", .args = {60.0f, 100.0f}});
    CHECK(decoded(got[2]).int32(0) == 1);

    c.write(post(R"([["/ok", 1], ["bad", 2]])", port));
    const auto refused = c.response();
    CHECK_THAT(refused, StartsWith("HTTP/1.1 400 "));
    CHECK_THAT(refused, ContainsSubstring("address bad does not start with /"));
    CHECK(bridge->takeMessages().empty());
    CHECK(bridge->counts().received == 3);
}

TEST_CASE("posts need this page port's own Origin (io.md 12.7)") {
    const site s;
    const auto bridge = listening(s);
    const auto port = bridge->port();
    for (const std::string& origin :
         {std::string(), std::string("http://evil.example"), "https://localhost:" + std::to_string(port),
          "http://127.0.0.1:" + std::to_string(port)}) {
        INFO(origin);
        client c(port);
        c.write(post("[]", port, origin));
        CHECK_THAT(c.response(), StartsWith("HTTP/1.1 403 "));
    }
    CHECK(bridge->takeMessages().empty());
}

TEST_CASE("requests that break the HTTP rules get their status and close (io.md 12.11)") {
    const site s;
    const auto bridge = listening(s);
    const auto port = bridge->port();
    const std::string host = "Host: localhost:" + std::to_string(port) +
                             "\r\nOrigin: http://localhost:" + std::to_string(port) + "\r\n";
    const auto status = [&](const std::string& request) {
        client c(port);
        c.write(request);
        const auto r = c.response();
        CHECK(c.closed());
        return r.substr(0, 12);
    };
    CHECK(status("POST /osc HTTP/1.1\r\n" + host + "\r\n") == "HTTP/1.1 411");
    CHECK(status("POST /osc HTTP/1.1\r\n" + host + "Content-Length: 70000\r\n\r\n") == "HTTP/1.1 413");
    CHECK(status("BREW /pot HTTP/1.0\r\n\r\n") == "HTTP/1.1 400");
    client put(port);
    put.write("PUT /osc HTTP/1.1\r\n" + host + "Content-Length: 2\r\n\r\n[]");
    CHECK_THAT(put.response(), StartsWith("HTTP/1.1 405 "));
}

TEST_CASE("a request whose Connection options include close is closed after its response (RFC 9110 7.6.1)") {
    const site s;
    const auto bridge = listening(s);
    const auto port = bridge->port();
    for (const std::string options : {"Close", "keep-alive, close", "CLOSE\t"}) {
        INFO(options);
        client c(port);
        c.write("GET /index.html HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                "\r\nConnection: " + options + "\r\n\r\n");
        CHECK_THAT(c.response(), StartsWith("HTTP/1.1 200 "));
        CHECK(c.closed());
    }
}

TEST_CASE("messages for pages arrive on GET /osc as server-sent events (io.md 12.4 and 12.5)") {
    const site s;
    const auto bridge = listening(s);
    client stream(bridge->port());
    stream.write(get("/osc", bridge->port()));
    CHECK_THAT(stream.readUntil([](const std::string& b) { return b.find("\r\n\r\n") != std::string::npos; }),
               ContainsSubstring("Content-Type: text/event-stream"));
    // The stream is registered once its request is read.
    CHECK(support::waitUntil([&] {
        bridge->send(protocol::encode({.address = "/ping", .args = {}}));
        return stream
                   .readUntil(
                       [](const std::string& b) { return b.find("data: [\"/ping\"]") != std::string::npos; })
                   .find("data: [\"/ping\"]") != std::string::npos;
    }));
    bridge->send(protocol::encode({.address = "/level", .args = {0.25f}}));
    CHECK_THAT(stream.readUntil([](const std::string& b) { return b.find("/level") != std::string::npos; }),
               ContainsSubstring("data: [\"/level\",0.25]\n\n"));
}

TEST_CASE("a page that connects first receives the last message sent to each address") {
    const site s;
    const auto bridge = listening(s);
    bridge->follow(std::make_shared<const pagePlan>(
        pagePlan{.addresses = {"/loops/bass/playing", "/level"}, .banks = {}}));
    // Sent while no page listens: two to one address, one to another, and a reload.
    bridge->send(protocol::encode({.address = "/loops/bass/playing", .args = {1.0f}}));
    bridge->send(protocol::encode({.address = "/loops/bass/playing", .args = {2.0f}}));
    bridge->send(protocol::encode({.address = "/level", .args = {0.5f}}));
    bridge->send(protocol::encode({.address = std::string(page::reloadAddress), .args = {}}));
    client stream(bridge->port());
    stream.write(get("/osc", bridge->port()));
    const auto got = stream.readUntil([](const std::string& b) {
        return b.find("/level") != std::string::npos && b.find("/playing") != std::string::npos;
    });
    CHECK_THAT(got, ContainsSubstring("data: [\"/loops/bass/playing\",2.0]"));
    CHECK_THAT(got, !ContainsSubstring("[\"/loops/bass/playing\",1.0]"));
    CHECK_THAT(got, ContainsSubstring("data: [\"/level\",0.5]"));
    CHECK_THAT(got, !ContainsSubstring(std::string(page::reloadAddress)));
}

TEST_CASE("the messages kept for pages are the active plan's addresses' alone, all of them") {
    const site s;
    const auto bridge = listening(s);
    std::set<std::string> many;
    for (int i = 0; i < 400; ++i) many.insert("/v/" + std::to_string(i));
    bridge->follow(std::make_shared<const pagePlan>(pagePlan{.addresses = many, .banks = {}}));
    for (int i = 0; i < 400; ++i)
        bridge->send(protocol::encode({.address = "/v/" + std::to_string(i), .args = {1.0f}}));
    bridge->send(protocol::encode({.address = "/other", .args = {1.0f}}));
    {
        client stream(bridge->port());
        stream.write(get("/osc", bridge->port()));
        // More than a connection's queue holds, all of them.
        const auto got =
            stream.readUntil([](const std::string& b) { return b.find("/v/399\"") != std::string::npos; });
        CHECK_THAT(got, ContainsSubstring("data: [\"/v/0\",1.0]"));
        CHECK_THAT(got, ContainsSubstring("data: [\"/v/399\",1.0]"));
        CHECK_THAT(got, !ContainsSubstring("/other"));
    }
    // Another piece on the same port: the first piece's values go.
    bridge->follow(std::make_shared<const pagePlan>(pagePlan{.addresses = {"/v/1", "/next"}, .banks = {}}));
    bridge->send(protocol::encode({.address = "/next", .args = {2.0f}}));
    client stream(bridge->port());
    stream.write(get("/osc", bridge->port()));
    const auto got =
        stream.readUntil([](const std::string& b) { return b.find("/next") != std::string::npos; });
    CHECK_THAT(got, ContainsSubstring("data: [\"/v/1\",1.0]"));
    CHECK_THAT(got, !ContainsSubstring("/v/2\""));
}

TEST_CASE("the engine host names the addresses its plan sends to the page port, and its banks") {
    primitives::engine audio({.audio = {.sampleRate = 48000.0, .blockSize = 64}, .pipes = {}});
    engineHost host(audio);
    support::graph g;
    g.spec.pagePort = 8000;
    g.pipe(support::control(0));
    for (const auto& [id, port, address] : {std::tuple{"a", 8000.0, "/a"}, std::tuple{"b", 9000.0, "/b"}}) {
        auto& n = g.node(id, "oscOut");
        n.params = {{"port", port}, {"address", std::string(address)}};
        n.inputs["value"] = {support::control(0)};
    }
    auto& d = g.node("d", "oscOut");
    d.params = {{"port", 8000.0}};
    d.inputs["value"] = {support::control(0)};
    REQUIRE_FALSE(host.loadPlan(1, ir::write(g.spec)));
    host.play(1, 0.0f, "none");
    std::vector<float> l(64), r(64);
    const std::array outs = {std::span(l), std::span(r)};
    audio.process({.inputs = {}, .outputs = outs}, 64, 0.0);
    host.poll();
    REQUIRE(host.pageNames());
    CHECK(host.pageNames()->addresses == std::set<std::string>{"/a", "/value"});
}

TEST_CASE("the page port holds at most 16 connections and refuses more with 503 (io.md 12.11)") {
    const site s;
    const auto bridge = listening(s);
    std::vector<std::unique_ptr<client>> held;
    for (std::size_t i = 0; i < pageBridge::maxConnections; ++i) {
        held.push_back(std::make_unique<client>(bridge->port()));
        held.back()->write(get("/", bridge->port()));
        CHECK_THAT(held.back()->response(), StartsWith("HTTP/1.1 200 "));  // accepted and answered
    }
    client extra(bridge->port());
    CHECK_THAT(extra.response(), StartsWith("HTTP/1.1 503 "));
    held.pop_back();  // one goes; another may come
    CHECK(support::waitUntil([&] {
        client again(bridge->port());
        again.write(get("/", bridge->port()));
        return again.response().starts_with("HTTP/1.1 200 ");
    }));
}

TEST_CASE("the listener closes and moves with the page port asked for (io.md 12.2)") {
    const site s;
    pageBridge bridge({.root = s.path(), .helper = {}, .report = {}});
    CHECK(bridge.port() == 0);
    bridge.listen(0);
    const auto first = bridge.port();
    REQUIRE(first != 0);
    client held(first);
    held.write(get("/", first));
    CHECK_THAT(held.response(), StartsWith("HTTP/1.1 200 "));
    bridge.listen(std::nullopt);
    CHECK(bridge.port() == 0);
    CHECK(held.closed());  // its pages go with it
    CHECK_FALSE(client(first).connected());
}

TEST_CASE("the engine host reports the page port of the plan that plays (io.md 12.2)") {
    primitives::engine audio({.audio = {.sampleRate = 48000.0, .blockSize = 64}, .pipes = {}});
    engineHost host(audio);
    support::graph g;
    g.spec.pagePort = 8000;
    REQUIRE_FALSE(host.loadPlan(1, ir::write(g.spec)));
    REQUIRE_FALSE(host.loadPlan(2, ir::write(support::graph{}.spec)));
    const auto run = [&] {
        std::vector<float> l(64), r(64);
        const std::array outs = {std::span(l), std::span(r)};
        audio.process({.inputs = {}, .outputs = outs}, 64, 0.0);
        host.poll();
    };
    CHECK_FALSE(host.pagePort());
    host.play(1, 0.0f, "none");
    run();
    CHECK(host.pagePort() == 8000);
    CHECK_FALSE(host.page());    // a plan without a layout draws no page
    host.play(2, 0.0f, "none");  // a plan without a page
    run();
    run();
    CHECK_FALSE(host.pagePort());
}

TEST_CASE("the engine host draws the page of a plan with a layout (dsl-ui.md 7.1)") {
    primitives::engine audio({.audio = {.sampleRate = 48000.0, .blockSize = 64}, .pipes = {}});
    engineHost host(audio);
    support::graph g;
    g.spec.pagePort = 8000;
    g.spec.layout = metadata::pageNode{.kind = "page",
                                       .label = {},
                                       .address = {},
                                       .params = {},
                                       .children = {metadata::pageNode{.kind = "panel",
                                                                       .label = "Synth",
                                                                       .address = {},
                                                                       .params = {},
                                                                       .children = {},
                                                                       .src = std::nullopt}},
                                       .src = std::nullopt};
    REQUIRE_FALSE(host.loadPlan(1, ir::write(g.spec)));
    host.play(1, 0.0f, "none");
    std::vector<float> l(64), r(64);
    const std::array outs = {std::span(l), std::span(r)};
    audio.process({.inputs = {}, .outputs = outs}, 64, 0.0);
    host.poll();
    REQUIRE(host.page());
    CHECK_THAT(*host.page(), ContainsSubstring("<es-panel label=\"Synth\">"));
}

namespace {

constexpr std::array<std::uint8_t, 4> mask{0x12, 0x34, 0x56, 0x78};

// A page's socket on /osc: the handshake, then frames each way.
class socketClient {
public:
    explicit socketClient(std::uint16_t port, std::string_view query = {}) : c_(port) {
        const std::string host = "localhost:" + std::to_string(port);
        c_.write("GET /osc" + std::string(query) + " HTTP/1.1\r\nHost: " + host + "\r\nOrigin: http://" +
                 host +
                 "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
        handshake_ =
            c_.readUntil([](const std::string& b) { return b.find("\r\n\r\n") != std::string::npos; });
        c_.take();
        const auto end = handshake_.find("\r\n\r\n");
        if (end != std::string::npos) {
            reader_.feed(handshake_.substr(end + 4), [&](const frameMessage& m) { got_.push_back(m); });
            handshake_.resize(end + 4);
        }
    }

    const std::string& handshake() const noexcept { return handshake_; }
    void send(opcode op, std::string_view payload, bool masked = true) {
        c_.write(encodeFrame(op, payload, true, masked ? std::optional(mask) : std::nullopt));
    }
    // The next frame from the server, past pings unless @p pings, waiting
    // for it; nothing when the connection closes first.
    std::optional<frameMessage> next(bool pings = false) {
        const auto deadline = std::chrono::steady_clock::now() + support::settleTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            while (!got_.empty()) {
                auto m = std::move(got_.front());
                got_.erase(got_.begin());
                if (!pings && m.op == opcode::ping) continue;
                return m;
            }
            c_.readUntil([](const std::string& b) { return !b.empty(); });
            const std::string bytes = c_.take();
            if (bytes.empty()) return std::nullopt;  // closed, or nothing for the whole wait
            reader_.feed(bytes, [&](const frameMessage& m) { got_.push_back(m); });
        }
        return std::nullopt;
    }
    bool closed() { return c_.closed(); }

private:
    client c_;
    std::string handshake_;
    frameReader reader_{false, std::size_t{1} << 24U};
    std::vector<frameMessage> got_;
};

std::uint16_t statusOf(const frameMessage& close) {
    const auto byte = [&](std::size_t i) {
        return static_cast<unsigned>(static_cast<std::uint8_t>(close.payload.at(i)));
    };
    return static_cast<std::uint16_t>((byte(0) << 8U) | byte(1));
}

std::string bytesOf(const protocol::message& m) {
    const auto b = protocol::encode(m);
    return {b.begin(), b.end()};
}

}  // namespace

TEST_CASE("a socket on /osc carries messages both ways (io.md 12.6)") {
    const site s;
    const auto bridge = listening(s);
    socketClient page(bridge->port());
    CHECK_THAT(page.handshake(), StartsWith("HTTP/1.1 101 Switching Protocols\r\n"));
    CHECK_THAT(page.handshake(), ContainsSubstring("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"));
    page.send(opcode::text, R"([["/cutoff", 0.4], ["/note", 60, 100]])");
    page.send(opcode::binary, bytesOf({.address = "/raw", .args = {std::int32_t{7}}}));
    std::vector<oscBytes> got;
    CHECK(support::waitUntil([&] {
        for (auto& m : bridge->takeMessages()) got.push_back(std::move(m));
        return got.size() == 3;
    }));
    REQUIRE(got.size() == 3);
    CHECK(decoded(got[0]).address == "/cutoff");
    CHECK(decoded(got[1]).address == "/note");
    CHECK(decoded(got[2]) == protocol::message{.address = "/raw", .args = {std::int32_t{7}}});

    bridge->send(protocol::encode({.address = "/level", .args = {0.25f}}));
    const auto frame = support::unwrap(page.next());
    CHECK(frame.op == opcode::text);
    CHECK(frame.payload == R"([["/level",0.25]])");
}

TEST_CASE("a socket asked for binary frames gets each message as OSC (io.md 12.6)") {
    const site s;
    const auto bridge = listening(s);
    socketClient page(bridge->port(), "?format=binary");
    REQUIRE_THAT(page.handshake(), StartsWith("HTTP/1.1 101 "));
    bridge->send(protocol::encode({.address = "/level", .args = {0.5f}}));
    const auto m = support::unwrap(page.next());
    CHECK(m.op == opcode::binary);
    const oscBytes bytes(m.payload.begin(), m.payload.end());
    CHECK(decoded(bytes) == protocol::message{.address = "/level", .args = {0.5f}});
}

TEST_CASE("a socket needs this port's Origin and version 13 (io.md 12.6 and 12.7)") {
    const site s;
    const auto bridge = listening(s);
    const auto port = bridge->port();
    const std::string host = "localhost:" + std::to_string(port);
    const auto answer = [&](const std::string& headers) {
        client c(port);
        c.write("GET /osc HTTP/1.1\r\nHost: " + host + "\r\nUpgrade: websocket\r\n" + headers +
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
        return c.response().substr(0, 12);
    };
    CHECK(answer("Sec-WebSocket-Version: 13\r\n") == "HTTP/1.1 403");
    CHECK(answer("Origin: http://evil.example\r\nSec-WebSocket-Version: 13\r\n") == "HTTP/1.1 403");
    CHECK(answer("Origin: http://" + host + "\r\nSec-WebSocket-Version: 8\r\n") == "HTTP/1.1 400");
}

TEST_CASE(
    "a socket closes on an unmasked frame with 1002 and on a message over 64 KiB with 1009 (io.md 12.6)") {
    const site s;
    const auto bridge = listening(s);
    {
        socketClient page(bridge->port());
        page.send(opcode::text, "[]", false);
        const auto close = support::unwrap(page.next());
        CHECK(close.op == opcode::close);
        CHECK(statusOf(close) == 1002);
        CHECK(page.closed());
    }
    {
        socketClient page(bridge->port());
        page.send(opcode::text, std::string(maxPageBody + 1, ' '));
        const auto close = support::unwrap(page.next());
        CHECK(close.op == opcode::close);
        CHECK(statusOf(close) == 1009);
    }
    // A frame that breaks a message rule is dropped; the socket stays.
    socketClient page(bridge->port());
    page.send(opcode::text, R"([["bad", 1]])");
    page.send(opcode::binary, "no address");
    page.send(opcode::text, R"([["/fine", 1]])");
    std::vector<oscBytes> got;
    CHECK(support::waitUntil([&] {
        for (auto& m : bridge->takeMessages()) got.push_back(std::move(m));
        return !got.empty();
    }));
    REQUIRE(got.size() == 1);
    CHECK(decoded(got[0]).address == "/fine");
    CHECK(bridge->counts().dropped == 2);
}

TEST_CASE("a socket's pings are answered and a page that leaves two unanswered is closed (io.md 12.6)") {
    const site s;
    pageBridge bridge(
        {.root = s.path(), .helper = {}, .report = {}, .pingInterval = std::chrono::milliseconds(50)});
    bridge.listen(0);
    socketClient page(bridge.port());
    page.send(opcode::ping, "hi");
    auto m = support::unwrap(page.next());
    CHECK(m.op == opcode::pong);
    CHECK(m.payload == "hi");
    m = support::unwrap(page.next(true));
    CHECK(m.op == opcode::ping);
    page.send(opcode::pong, "");  // answered: open through more pings
    CHECK(support::unwrap(page.next(true)).op == opcode::ping);
    // No more answers: two pings later the server closes it.
    CHECK(page.closed());
}

TEST_CASE("a page that reads too slowly loses only its own messages (io.md 12.6)") {
    const site s;
    const auto bridge = listening(s);
    socketClient slow(bridge->port());
    std::atomic<std::size_t> heard{0};
    {
        socketClient fast(bridge->port());
        espresso::jthread reader([&](const espresso::stop_token& stop) {
            while (!stop.stop_requested()) {
                const auto m = fast.next();
                if (!m) return;
                std::size_t n = 0;
                for (auto at = m->payload.find("[\"/n\""); at != std::string::npos;
                     at = m->payload.find("[\"/n\"", at + 1))
                    ++n;
                heard += n;
            }
        });
        // Paced in batches smaller than a connection's queue, each heard
        // before the next, so neither the shared outbox nor the fast page's
        // queue fills; the slow page's socket and queue do.
        constexpr std::size_t total = 60000;
        constexpr std::size_t batch = 200;
        static_assert(batch < pageBridge::connectionQueue);
        for (std::size_t sent = 0; sent < total; sent += batch) {
            for (std::size_t i = 0; i < batch; ++i)
                bridge->send(
                    protocol::encode({.address = "/n", .args = {static_cast<std::int32_t>(sent + i)}}));
            REQUIRE(support::waitUntil([&] { return heard.load() >= sent + batch; }));
        }
        CHECK(heard.load() == total);
        reader.request_stop();
        bridge->listen(std::nullopt);  // closes both, ending the reader's wait
    }
    CHECK(bridge->counts().dropped > 0);
}

TEST_CASE("the page port serves the page helper at /espresso.js (io.md 12.9)") {
    const site s;
    const auto bridge = listening(s, std::string(pageHelper()));
    client c(bridge->port());
    c.write(get("/espresso.js", bridge->port()));
    const auto r = c.response();
    CHECK_THAT(r, StartsWith("HTTP/1.1 200 "));
    CHECK_THAT(r, ContainsSubstring("text/javascript"));
    const auto text =
        support::readText(std::filesystem::path(ESPRESSO_SOURCE_DIR) / "src/apps/server/espresso.js");
    CHECK(pageHelper() == text);
    CHECK(r.ends_with(text));
}

TEST_CASE("the page port serves the control library at /espresso-ui.js (ui.md 4.1)") {
    const site s;
    const auto bridge = listening(s, std::string(pageHelper()), std::string(pageControls()));
    client c(bridge->port());
    c.write(get("/espresso-ui.js", bridge->port()));
    const auto r = c.response();
    CHECK_THAT(r, StartsWith("HTTP/1.1 200 "));
    CHECK_THAT(r, ContainsSubstring("text/javascript"));
    const auto text =
        support::readText(std::filesystem::path(ESPRESSO_SOURCE_DIR) / "src/apps/server/espresso-ui.js");
    CHECK(pageControls() == text);
    CHECK(r.ends_with(text));
    // A server without it answers 404, as for any missing file.
    const auto bare = listening(s);
    client other(bare->port());
    other.write(get("/espresso-ui.js", bare->port()));
    CHECK_THAT(other.response(), StartsWith("HTTP/1.1 404 "));
}
