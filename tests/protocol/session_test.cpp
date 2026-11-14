#include <catch2/catch_test_macros.hpp>

#include <proj/protocol/client.h>
#include <proj/protocol/crc32.h>
#include <proj/protocol/irTransfer.h>
#include <proj/protocol/jthread.h>
#include <proj/protocol/messages.h>
#include <proj/protocol/serverSession.h>

#include <atomic>
#include <bit>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace protocol;

namespace {

class recordingEngine : public engine {
public:
    std::optional<std::string> loadPlan(std::int32_t planId, std::string ir) override {
        if (ir == "bad") return "bad IR";
        plans[planId] = std::move(ir);
        return std::nullopt;
    }
    bool hasPlan(std::int32_t planId) const override { return plans.count(planId) != 0; }
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
    return {std::string(addr), std::move(args)};
}

// The single reply to a request.
message one(const std::vector<outgoing>& out) {
    REQUIRE(out.size() == 1);
    return out[0].msg;
}

serverInfo info() { return {"test-server 1", "meta-1", 48000, 256}; }

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
    CHECK(std::string(*r.string(2)).find("meta-0") != std::string::npos);
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
    auto server = udpSocket::bind(0, error);
    REQUIRE(server);
    std::atomic<int> dropFirstEnd{1};
    std::mutex sessionMutex;
    proj::jthread loop([&](const proj::stop_token& stop) {
        while (!stop.stop_requested()) {
            auto d = server->receive(10);
            if (!d) continue;
            std::lock_guard lock(sessionMutex);
            const auto m = decode(d->bytes);
            // Lose the first /ir.end to exercise the retry path.
            if (m && m->address == address::irEnd && dropFirstEnd-- > 0) continue;
            for (const auto& o : session.handleDatagram(d->bytes, d->from)) {
                server->send(encode(o.msg), o.to);
            }
        }
    });

    std::mutex seenMutex;
    std::vector<message> unsolicited;
    client c({server->port(), 50, 3}, [&](const message& m) {
        std::lock_guard lock(seenMutex);
        unsolicited.push_back(m);
    });
    REQUIRE(c.open(error));

    auto h = c.request(address::hello, {std::string("client"), std::string("meta-1")});
    REQUIRE(h.ok());
    CHECK(h.reply->string(1) == "test-server 1");
    REQUIRE(c.request(address::notify, {std::int32_t{1}}).ok());

    std::string payload;
    for (int i = 0; i < 150'000; ++i) payload.push_back(static_cast<char>(i % 251));
    CHECK_FALSE(c.submitIr(5, payload));
    {
        std::lock_guard lock(sessionMutex);
        CHECK(e.plans.at(5) == payload);
    }
    CHECK(c.request(address::play, {std::int32_t{5}, 0.05f, std::string("none")}).ok());
    const auto bad = c.request(address::play, {std::int32_t{6}, 0.05f, std::string("none")});
    CHECK(bad.failCode() == errorCode::unknownPlan);

    {
        std::lock_guard lock(sessionMutex);
        for (const auto& o : session.broadcast(msg(address::planActive, {std::int32_t{5}}))) {
            server->send(encode(o.msg), o.to);
        }
    }
    for (int i = 0; i < 100; ++i) {
        {
            std::lock_guard lock(seenMutex);
            if (!unsolicited.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    {
        std::lock_guard lock(seenMutex);
        REQUIRE(unsolicited.size() == 1);
        CHECK(unsolicited[0] == msg(address::planActive, {std::int32_t{5}}));
    }

    loop.request_stop();
    loop.join();
}

TEST_CASE("requests fail after the retries when no server answers (protocol.md 3.2)",
          "[protocol][client]") {
    auto unused = udpSocket::bind(0);  // bound, so nothing else takes the port, but never replies
    REQUIRE(unused);
    client c({unused->port(), 10, 2});
    std::string openError;
    REQUIRE(c.open(openError));
    const auto start = std::chrono::steady_clock::now();
    const auto r = c.request(address::sync);
    CHECK_FALSE(r.reply);
    CHECK(r.error.find("after 3 attempts") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(30));
}
