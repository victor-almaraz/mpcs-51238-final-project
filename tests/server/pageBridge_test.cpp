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
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <espresso/ir/ir.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/io.h>
#include <espresso/protocol/jthread.h>
#include <espresso/protocol/osc.h>

#include "engineHost.h"
#include "fileHandle.h"
#include "oscBridge.h"
#include "pageBridge.h"
#include "pageHelper.h"
#include "pageMessages.h"
#include "support/graph.h"
#include "support/tempDir.h"
#include "support/timing.h"
#include "support/unwrap.h"
#include "webSocket.h"

#include <poll.h>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using namespace server;

namespace {

// A connection to the page port, written and read as raw HTTP.
class client {
public:
    explicit client(std::uint16_t port) : fd_(::socket(AF_INET, SOCK_STREAM, 0)) {
        REQUIRE(fd_);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): POSIX takes the generic address type
        connected_ = ::connect(fd_.get(), reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0;
    }

    bool connected() const noexcept { return connected_; }

    void write(std::string_view bytes) const {
        while (!bytes.empty()) {
            const auto n = ::send(fd_.get(), bytes.data(), bytes.size(), 0);
            REQUIRE(n > 0);
            bytes.remove_prefix(static_cast<std::size_t>(n));
        }
    }

    // Reads until @p done holds for what has arrived, the peer closes, or
    // the wait times out.
    template <typename Done>
    std::string readUntil(Done done) {
        const auto deadline = std::chrono::steady_clock::now() + support::settleTimeout;
        while (!done(buffer_) && std::chrono::steady_clock::now() < deadline) {
            pollfd p{.fd = fd_.get(), .events = POLLIN, .revents = 0};
            if (::poll(&p, 1, 20) <= 0) continue;
            std::array<char, 8192> chunk{};
            const auto n = ::read(fd_.get(), chunk.data(), chunk.size());
            if (n <= 0) {
                closed_ = true;
                break;
            }
            buffer_.append(chunk.data(), static_cast<std::size_t>(n));
        }
        return buffer_;
    }

    // The next whole response: its head and a Content-Length body.
    std::string response() {
        const auto whole = [](const std::string& b) {
            const auto end = b.find("\r\n\r\n");
            if (end == std::string::npos) return false;
            const auto at = b.find("Content-Length: ");
            if (at == std::string::npos || at > end) return true;
            return b.size() >= end + 4 + std::stoul(b.substr(at + 16));
        };
        readUntil(whole);
        if (!whole(buffer_)) return std::exchange(buffer_, {});
        const auto end = buffer_.find("\r\n\r\n");
        const auto at = buffer_.find("Content-Length: ");
        const std::size_t length =
            at == std::string::npos || at > end ? 0 : std::stoul(buffer_.substr(at + 16));
        std::string out = buffer_.substr(0, end + 4 + length);
        buffer_.erase(0, out.size());
        return out;
    }

    // What has arrived and not been taken.
    std::string take() { return std::exchange(buffer_, {}); }

    bool closed() {
        readUntil([](const std::string&) { return false; });
        return closed_;
    }

private:
    fileHandle fd_;
    bool connected_ = false;
    bool closed_ = false;
    std::string buffer_;
};

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

std::unique_ptr<pageBridge> listening(const site& s, std::string helper = {}) {
    auto b = std::make_unique<pageBridge>(
        pageOptions{.root = s.path(), .helper = std::move(helper), .report = {}});
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
    host.play(2, 0.0f, "none");  // a plan without a page
    run();
    run();
    CHECK_FALSE(host.pagePort());
}

TEST_CASE("OSC output to the page port goes to pages and not to UDP (io.md 12.3)") {
    support::graph g;
    g.pipe(support::events(0)).pipe(support::control(1));
    auto& lfo = g.node("lfo", "lfo");
    lfo.params = {{"depth", 0.0}, {"offset", 0.5}};
    lfo.outputs["out"] = {support::control(1)};
    auto& out = g.node("send", "oscOut");
    out.params = {{"port", 8123.0}, {"address", std::string("/level")}};
    out.inputs["value"] = {support::control(1)};
    support::renderer r;
    r.play(g.spec);
    r.run(64);
    oscBridge bridge;
    std::ostringstream errors;
    std::vector<std::uint16_t> diverted;
    bridge.pump(r.engine.io(), 0.0, errors, std::nullopt,
                [&](std::uint16_t port, const primitives::oscPacket&) {
                    diverted.push_back(port);
                    return port == 8123;
                });
    CHECK(diverted == std::vector<std::uint16_t>{8123});
    CHECK(r.engine.io().drops() == 0);  // taken, not sent and not dropped
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
    std::ifstream file(std::filesystem::path(ESPRESSO_SOURCE_DIR) / "src/apps/server/espresso.js");
    std::ostringstream text;
    text << file.rdbuf();
    CHECK(pageHelper() == text.str());
    CHECK(r.ends_with(text.str()));
}
