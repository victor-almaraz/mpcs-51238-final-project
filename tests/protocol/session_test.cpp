#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <proj/protocol/client.h>
#include <proj/protocol/crc32.h>
#include <proj/protocol/irTransfer.h>
#include <proj/protocol/jthread.h>
#include <proj/protocol/messages.h>
#include <proj/protocol/osc.h>
#include <proj/protocol/serverSession.h>
#include <proj/protocol/udp.h>

#include "support/timing.h"
#include "support/unwrap.h"

using namespace protocol;

namespace {

class recordingEngine : public engine {
public:
    std::optional<std::string> loadPlan(std::int32_t planId, std::string ir) override {
        if (ir == "bad") return "bad IR";
        plans[planId] = std::move(ir);
        return std::nullopt;
    }
    bool hasPlan(std::int32_t planId) const override { return plans.contains(planId); }
    void play(std::int32_t planId, float, std::string_view quant) override {
        ++plays;
        lastQuant = std::string(quant);
        active = planId;
    }
    void stop(float) override { ++stops; }
    void setControl(std::int32_t index, float value) override { controls[index] = value; }
    float control(std::int32_t index) const override {
        const auto it = controls.find(index);
        return it == controls.end() ? 0.0f : it->second;
    }
    serverCounters counters() const override {
        serverCounters c;
        c.activePlan = active;
        c.xruns = 2;
        return c;
    }
    void quit() override { quitCalled = true; }

    std::map<std::int32_t, std::string> plans;
    std::map<std::int32_t, float> controls;
    int plays = 0;
    int stops = 0;
    std::int32_t active = 0;
    std::string lastQuant;
    bool quitCalled = false;
};

const endpoint alice{40001};
const endpoint bob{40002};

message msg(std::string_view addr, std::vector<argument> args) {
    return {.address = std::string(addr), .args = std::move(args)};
}

// The single reply to a request.
message one(const std::vector<outgoing>& out) {
    REQUIRE(out.size() == 1);
    return out[0].msg;
}

serverInfo info() {
    return {
        .serverVersion = "test-server 1", .metadataVersion = "meta-1", .sampleRate = 48000, .blockSize = 256};
}

void hello(serverSession& s, endpoint who, std::int32_t id = 1) {
    REQUIRE(one(s.handle(msg(address::hello, {id, std::string("c"), std::string("meta-1")}), who))
                .address == address::helloOk);
}

constexpr std::int32_t code(errorCode c) { return static_cast<std::int32_t>(c); }

} // namespace

TEST_CASE("clients must send /hello first (protocol.md 4.1)", "[protocol][session]") {
    recordingEngine e;
    serverSession s(info(), e);
    auto r = one(s.handle(msg(address::status, {std::int32_t{5}}), alice));
    CHECK(r.address == address::fail);
    CHECK(r.int32(0) == 5);
    CHECK(r.int32(1) == code(errorCode::notConnected));

    r = one(s.handle(
        msg(address::hello, {std::int32_t{6}, std::string("0.1"), std::string("meta-1")}), alice));
    CHECK(r == msg(address::helloOk, {std::int32_t{6}, std::string("test-server 1"),
                                      std::int32_t{48000}, std::int32_t{256}}));
    CHECK(s.isConnected(alice));
    CHECK_FALSE(s.isConnected(bob));
    CHECK(one(s.handle(msg(address::sync, {std::int32_t{7}}), alice)) ==
          msg(address::synced, {std::int32_t{7}}));
}

TEST_CASE("a metadata version mismatch fails /hello (protocol.md 4.2)", "[protocol][session]") {
    recordingEngine e;
    serverSession s(info(), e);
    const auto r = one(s.handle(
        msg(address::hello, {std::int32_t{1}, std::string("0.1"), std::string("meta-0")}), alice));
    CHECK(r.int32(1) == code(errorCode::versionMismatch));
    CHECK(std::string(support::unwrap(r.string(2))).find("meta-0") != std::string::npos);
    CHECK_FALSE(s.isConnected(alice));
}

TEST_CASE("repeated request IDs get the original reply once (protocol.md 3.3)",
          "[protocol][session]") {
    recordingEngine e;
    e.plans[4] = "{}";
    serverSession s(info(), e);
    hello(s, alice);
    const auto play =
        msg(address::play, {std::int32_t{2}, std::int32_t{4}, 0.05f, std::string("bar")});
    const auto first = one(s.handle(play, alice));
    const auto again = one(s.handle(play, alice));
    CHECK(first == msg(address::done, {std::int32_t{2}}));
    CHECK(again == first);
    CHECK(e.plays == 1);
    CHECK(e.lastQuant == "bar");
}

TEST_CASE("session validates addresses and arguments", "[protocol][session]") {
    recordingEngine e;
    serverSession s(info(), e);
    hello(s, alice);
    auto codeOf = [&](const message& m) { return one(s.handle(m, alice)).int32(1); };
    CHECK(codeOf(msg("/nope", {std::int32_t{3}})) == code(errorCode::unknownAddress));
    CHECK(codeOf(msg(address::play, {std::int32_t{4}, std::string("x")})) ==
          code(errorCode::malformedArguments));
    CHECK(codeOf(msg(address::play,
                     {std::int32_t{5}, std::int32_t{1}, 0.0f, std::string("half")})) ==
          code(errorCode::malformedArguments));
    CHECK(codeOf(msg(address::play,
                     {std::int32_t{6}, std::int32_t{99}, 0.0f, std::string("none")})) ==
          code(errorCode::unknownPlan));
    CHECK(codeOf(msg(address::status, {})) == code(errorCode::malformedArguments));

    CHECK(s.handleDatagram(std::vector<std::uint8_t>{1, 2, 3}, alice).empty());
    CHECK(s.droppedDatagrams() == 1);
}

TEST_CASE("IR transfer through the session (protocol.md 6)", "[protocol][session]") {
    recordingEngine e;
    serverSession s(info(), e);
    hello(s, alice);
    const std::string payload(40'000, 'q');
    const auto chunks = splitChunks(payload);
    auto chunk = [&](std::int32_t i) {
        const auto c = chunks[static_cast<std::size_t>(i)];
        return msg(address::irChunk,
                   {std::int32_t{3}, i, blob{std::vector<std::uint8_t>(c.begin(), c.end())}});
    };
    const auto crc = std::bit_cast<std::int32_t>(crc32(payload));
    CHECK(one(s.handle(msg(address::irBegin, {std::int32_t{10}, std::int32_t{3},
                                              std::int64_t{40'000}, std::int32_t{2}, crc}),
                       alice))
              .address == address::done);
    CHECK(s.handle(chunk(1), alice).empty());  // chunks get no reply

    auto r = one(s.handle(msg(address::irEnd, {std::int32_t{11}, std::int32_t{3}}), alice));
    CHECK(r == msg(address::irMissing, {std::int32_t{11}, std::int32_t{3}, std::int32_t{0}}));
    s.handle(chunk(0), alice);
    r = one(s.handle(msg(address::irEnd, {std::int32_t{12}, std::int32_t{3}}), alice));
    CHECK(r.address == address::done);
    CHECK(e.plans.at(3) == payload);

    // Invalid IR is reported with code 7.
    s.handle(msg(address::irBegin, {std::int32_t{13}, std::int32_t{4}, std::int64_t{3},
                                    std::int32_t{1}, std::bit_cast<std::int32_t>(crc32("bad"))}),
             alice);
    s.handle(msg(address::irChunk, {std::int32_t{4}, std::int32_t{0}, blob{{'b', 'a', 'd'}}}),
             alice);
    r = one(s.handle(msg(address::irEnd, {std::int32_t{14}, std::int32_t{4}}), alice));
    CHECK(r.int32(1) == code(errorCode::irInvalid));
}

TEST_CASE("notifications go to subscribed clients only (protocol.md 5)", "[protocol][session]") {
    recordingEngine e;
    serverSession s(info(), e);
    hello(s, alice);
    hello(s, bob);
    CHECK(s.countersMessages().empty());
    s.handle(msg(address::notify, {std::int32_t{2}, std::int32_t{1}}), alice);
    const auto out = s.countersMessages();
    REQUIRE(out.size() == 1);
    CHECK(out[0].to == alice);
    CHECK(out[0].msg.address == address::counters);
    CHECK(out[0].msg.int32(1) == 2);  // xruns
    s.handle(msg(address::notify, {std::int32_t{3}, std::int32_t{0}}), alice);
    CHECK(s.broadcast(msg(address::planActive, {std::int32_t{1}})).empty());
}

TEST_CASE("controls and quit", "[protocol][session]") {
    recordingEngine e;
    serverSession s(info(), e);
    hello(s, alice);
    CHECK(s.handle(msg(address::controlSet, {std::int32_t{3}, 0.5f, std::int32_t{4}, 1.5f}), alice)
              .empty());
    CHECK(one(s.handle(msg(address::controlGet, {std::int32_t{2}, std::int32_t{4}}), alice)) ==
          msg(address::controlValue, {std::int32_t{2}, std::int32_t{4}, 1.5f}));
    CHECK(one(s.handle(msg(address::quit, {std::int32_t{9}}), alice)).address == address::done);
    CHECK(e.quitCalled);
}

TEST_CASE("client and session over loopback UDP", "[protocol][client]") {
    recordingEngine e;
    serverSession session(info(), e);
    std::string error;
    auto server = support::unwrap(udpSocket::bind(0, error));
    std::atomic<int> dropFirstEnd{1};
    std::mutex sessionMutex;
    proj::jthread loop([&](const proj::stop_token& stop) {
        while (!stop.stop_requested()) {
            auto d = server.receive(10);
            if (!d) continue;
            std::scoped_lock lock(sessionMutex);
            const auto m = decode(d->bytes);
            // Lose the first /ir.end to exercise the retry path.
            if (m && m->address == address::irEnd && dropFirstEnd-- > 0) continue;
            for (const auto& o : session.handleDatagram(d->bytes, d->from)) {
                server.send(encode(o.msg), o.to);
            }
        }
    });

    std::mutex seenMutex;
    std::vector<message> unsolicited;
    client c({.serverPort = server.port(), .timeoutMs = support::replyTimeoutMs, .retries = 3},
             [&](const message& m) {
                 std::scoped_lock lock(seenMutex);
                 unsolicited.push_back(m);
             });
    REQUIRE(c.open(error));

    auto h = c.request(address::hello, {std::string("client"), std::string("meta-1")});
    REQUIRE(h.ok());
    CHECK(support::unwrap(h.reply).string(1) == "test-server 1");
    REQUIRE(c.request(address::notify, {std::int32_t{1}}).ok());

    std::string payload;
    for (int i = 0; i < 150'000; ++i) payload.push_back(static_cast<char>(i % 251));
    CHECK_FALSE(c.submitIr(5, payload));
    {
        std::scoped_lock lock(sessionMutex);
        CHECK(e.plans.at(5) == payload);
    }
    CHECK(c.request(address::play, {std::int32_t{5}, 0.05f, std::string("none")}).ok());
    const auto bad = c.request(address::play, {std::int32_t{6}, 0.05f, std::string("none")});
    CHECK(bad.failCode() == errorCode::unknownPlan);

    {
        std::scoped_lock lock(sessionMutex);
        for (const auto& o : session.broadcast(msg(address::planActive, {std::int32_t{5}}))) {
            server.send(encode(o.msg), o.to);
        }
    }
    for (int i = 0; i < 100; ++i) {
        {
            std::scoped_lock lock(seenMutex);
            if (!unsolicited.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    {
        std::scoped_lock lock(seenMutex);
        REQUIRE(unsolicited.size() == 1);
        CHECK(unsolicited[0] == msg(address::planActive, {std::int32_t{5}}));
    }
    // loop stops and joins when it goes out of scope, before the session
    // and the socket it uses, which are declared before it.
}

TEST_CASE("requests fail after the retries when no server answers (protocol.md 3.2)",
          "[protocol][client]") {
    // Bound, so nothing else takes the port, but never replies.
    const auto unused = support::unwrap(udpSocket::bind(0));
    client c({.serverPort = unused.port(), .timeoutMs = 10, .retries = 2});
    std::string openError;
    REQUIRE(c.open(openError));
    const auto start = std::chrono::steady_clock::now();
    const auto r = c.request(address::sync);
    CHECK_FALSE(r.reply);
    CHECK(r.error.find("after 3 attempts") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(30));
}

TEST_CASE("sockets report the receive buffer the system granted (protocol.md 6.1)", "[protocol][udp]") {
    auto socket = support::unwrap(udpSocket::bind(0));
    // At least the 64 KiB floor of the request; Linux may grant less than
    // requested, which the server reports with --verbose.
    CHECK(socket.receiveBufferBytes() >= 64 * 1024);
    auto moved = udpSocket(std::move(socket));
    CHECK(moved.receiveBufferBytes() >= 64 * 1024);
}

TEST_CASE("/play and /stop refuse fades that are not finite, negative, or too long (protocol.md 4, 8)",
          "[protocol][session]") {
    recordingEngine e;
    e.plans[4] = "{}";
    serverSession s(info(), e);
    hello(s, alice);
    std::int32_t id = 10;
    auto play = [&](argument fade) {
        return one(s.handle(msg(address::play, {id++, std::int32_t{4}, std::move(fade), std::string("none")}),
                            alice));
    };
    auto stop = [&](argument fade) {
        return one(s.handle(msg(address::stop, {id++, std::move(fade)}), alice));
    };
    const std::vector<argument> refused = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -0.5f,
        maxFadeSeconds * 2.0f,
        1e300,  // a double that narrows to an infinite float
    };
    for (const auto& fade : refused) {
        CHECK(play(fade).int32(1) == code(errorCode::malformedArguments));
        CHECK(stop(fade).int32(1) == code(errorCode::malformedArguments));
    }
    CHECK(e.plays == 0);
    CHECK(e.stops == 0);
    CHECK(stop(maxFadeSeconds).address == address::done);
    CHECK(play(0.0f).address == address::done);
}

TEST_CASE("dropped datagrams are reported in the drops counter (protocol.md 2.4, 5)", "[protocol][session]") {
    recordingEngine e;
    serverSession s(info(), e);
    hello(s, alice);
    s.handle(msg(address::notify, {std::int32_t{2}, std::int32_t{1}}), alice);
    CHECK(s.handleDatagram(std::vector<std::uint8_t>{1, 2, 3}, alice).empty());
    CHECK(s.handleDatagram(std::vector<std::uint8_t>{1, 2, 3, 4}, alice).empty());
    const auto out = s.countersMessages();
    REQUIRE(out.size() == 1);
    CHECK(out[0].msg.int32(2) == 2);  // missed deadlines, xruns, drops, ...
    const auto status = one(s.handle(msg(address::status, {std::int32_t{3}}), alice));
    CHECK(status.int32(4) == 2);  // request ID, load, missed deadlines, xruns, drops, ...
}

TEST_CASE("the session forgets the least recently heard client beyond maxClients (protocol.md 3.3)",
          "[protocol][session]") {
    recordingEngine e;
    serverSession s(info(), e);
    const auto port = [](std::size_t n) { return endpoint{static_cast<std::uint16_t>(50000 + n)}; };
    for (std::size_t n = 0; n < maxClients; ++n) hello(s, port(n));
    // Client 0 speaks again, so client 1 is now the least recently heard.
    CHECK(one(s.handle(msg(address::sync, {std::int32_t{2}}), port(0))).address == address::synced);
    hello(s, port(maxClients));
    CHECK(s.clientCount() == maxClients);
    CHECK(s.isConnected(port(0)));
    CHECK_FALSE(s.isConnected(port(1)));
    CHECK(s.isConnected(port(maxClients)));
}

namespace {

// Runs @p session on @p socket on a thread, through a serverLink, until
// destroyed. Callers lock mutex to use the session.
struct loopbackServer {
    loopbackServer(udpSocket& socket, serverSession& session)
        : link(socket, session), thread([this](const proj::stop_token& stop) {
              while (!stop.stop_requested()) {
                  const std::scoped_lock lock(mutex);
                  link.serveOnce(5);
              }
          }) {}

    std::mutex mutex;
    serverLink link;
    proj::jthread thread;  // last: stops before the link
};

}  // namespace

TEST_CASE("a destroyed client turns its notifications off (protocol.md 5)", "[protocol][client]") {
    recordingEngine e;
    serverSession session(info(), e);
    auto socket = support::unwrap(udpSocket::bind(0));
    loopbackServer server(socket, session);
    const auto subscribers = [&] {
        const std::scoped_lock lock(server.mutex);
        return session.broadcast(msg(address::planActive, {std::int32_t{1}})).size();
    };
    {
        client c({.serverPort = socket.port(), .timeoutMs = support::replyTimeoutMs, .retries = 3});
        std::string error;
        REQUIRE(c.open(error));
        REQUIRE(c.request(address::hello, {std::string("client"), std::string("meta-1")}).ok());
        REQUIRE(c.request(address::notify, {std::int32_t{1}}).ok());
        CHECK(subscribers() == 1);
    }
    for (int i = 0; i < 200 && subscribers() != 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(subscribers() == 0);
}

TEST_CASE("a large IR crosses a server receive buffer that holds few chunks (protocol.md 6.1, 6.3)",
          "[protocol][client]") {
    recordingEngine e;
    serverSession session(info(), e);
    auto server = support::unwrap(udpSocket::bind(0));
    std::mutex sessionMutex;
    // Models a receive buffer that holds chunksPerWindow chunks while the
    // server is busy: chunks beyond that, since the server last handled
    // another message, are lost.
    proj::jthread loop([&](const proj::stop_token& stop) {
        std::size_t burst = 0;
        while (!stop.stop_requested()) {
            auto d = server.receive(10);
            if (!d) continue;
            const auto m = decode(d->bytes);
            if (m && m->address == address::irChunk) {
                if (++burst > chunksPerWindow) continue;
            } else {
                burst = 0;
            }
            const std::scoped_lock lock(sessionMutex);
            for (const auto& o : session.handleDatagram(d->bytes, d->from)) server.send(encode(o.msg), o.to);
        }
    });

    client c({.serverPort = server.port(), .timeoutMs = support::replyTimeoutMs, .retries = 3});
    std::string error;
    REQUIRE(c.open(error));
    REQUIRE(c.request(address::hello, {std::string("client"), std::string("meta-1")}).ok());
    // 40 chunks: without pacing, each of the 5 rounds delivers only
    // chunksPerWindow of them.
    std::string payload;
    for (std::size_t i = 0; i < 40 * maxChunkBytes; ++i) payload.push_back(static_cast<char>(i % 253));
    CHECK(c.submitIr(8, payload) == std::nullopt);
    const std::scoped_lock lock(sessionMutex);
    CHECK(e.plans.count(8) == 1);
}

TEST_CASE("serverLink answers, counts undecodable datagrams, and sends counters (protocol.md 2.4, 5)",
          "[protocol][session]") {
    recordingEngine e;
    serverSession session(info(), e);
    auto server = support::unwrap(udpSocket::bind(0));
    auto peer = support::unwrap(udpSocket::bind(0));
    std::vector<std::string> seen;
    std::vector<endpoint> malformedFrom;
    trafficLog log;
    log.onMessage = [&](std::string_view direction, endpoint, const message& m) {
        seen.push_back(std::string(direction) + m.address);
    };
    log.onMalformed = [&](endpoint from) { malformedFrom.push_back(from); };
    serverLink link(server, session, std::move(log));
    const endpoint to{server.port()};
    auto exchange = [&](const std::vector<std::uint8_t>& bytes) {
        REQUIRE(peer.send(bytes, to));
        REQUIRE(link.serveOnce(1000));
    };
    auto next = [&] {
        const auto d = support::unwrap(peer.receive(1000));
        return support::unwrap(decode(d.bytes));
    };

    exchange(encode(msg(address::hello, {std::int32_t{1}, std::string("c"), std::string("meta-1")})));
    CHECK(next().address == address::helloOk);
    exchange({1, 2, 3, 4});
    CHECK(malformedFrom == std::vector<endpoint>{endpoint{peer.port()}});
    exchange(encode(msg(address::notify, {std::int32_t{2}, std::int32_t{1}})));
    CHECK(next().address == address::done);

    const auto now = std::chrono::steady_clock::now();
    link.sendCountersIfDue(now);
    const auto counters = next();
    CHECK(counters.address == address::counters);
    CHECK(counters.int32(2) == 1);  // the undecodable datagram
    link.sendCountersIfDue(now);    // not due again yet
    CHECK_FALSE(peer.receive(20));
    CHECK(seen == std::vector<std::string>{"<-/hello", "->/hello.ok", "<-/notify", "->/done", "->/counters"});
    CHECK_FALSE(link.serveOnce(1));  // nothing waiting
}

namespace {

// Uses about @p frames × 32 KiB of stack.
int deepFrames(int frames) {
    std::array<volatile int, std::size_t{8} * 1024> page{};  // 32 KiB
    page[0] = frames;
    return frames == 0 ? page[0] : deepFrames(frames - 1) + page[0];
}

}  // namespace

TEST_CASE("a proj::jthread has a stack of threadStackBytes on every platform") {
    static_assert(proj::threadStackBytes >= std::size_t{4} << 20u);
    int result = -1;
    {
        proj::jthread worker([&result] { result = deepFrames(128); });  // 4 MiB
    }
    CHECK(result == 128 * 129 / 2);
}

TEST_CASE("a UDP socket moved by assignment takes over the port and closes its own") {
    auto target = support::unwrap(protocol::udpSocket::bind(0));
    auto source = support::unwrap(protocol::udpSocket::bind(0));
    const auto port = source.port();
    target = std::move(source);
    CHECK(target.port() == port);
    auto rebound = protocol::udpSocket::bind(port);  // the port is still taken
    CHECK_FALSE(rebound.has_value());
}
