// The OSC bridge: UDP in and out over loopback, its bounds and failures,
// and pages as a second transport against a scripted page endpoint
// (io.md §12.2, §12.3).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/primitives/engine.h>
#include <espresso/primitives/io.h>
#include <espresso/protocol/osc.h>
#include <espresso/protocol/udp.h>

#include "oscBridge.h"
#include "pageEndpoint.h"
#include "pageMessages.h"
#include "support/graph.h"
#include "support/timing.h"
#include "support/unwrap.h"

using Catch::Matchers::StartsWith;
using namespace support;

namespace {

// Pages as the bridge sees them, scripted: the ports it listened on, the
// messages pages send it next, and the messages it sent them.
class scriptedPages final : public server::pageEndpoint {
public:
    std::vector<std::optional<std::uint16_t>> listened;
    std::vector<server::oscBytes> waiting;
    std::vector<server::oscBytes> sent;

    void listen(std::optional<std::uint16_t> port) override { listened.push_back(port); }
    std::vector<server::oscBytes> takeMessages() override { return std::exchange(waiting, {}); }
    void send(std::span<const std::uint8_t> osc) override { sent.emplace_back(osc.begin(), osc.end()); }
};

// The message @p address with one float, @p value.
server::oscBytes message(float value, std::string_view address = "/knob") {
    return protocol::encode({.address = std::string(address), .args = {value}});
}

primitives::oscPacket packetOf(const server::oscBytes& bytes) {
    return unwrap(primitives::oscPacket::of(bytes));
}

// A free port: bound, read, and closed at once.
std::uint16_t freePort() { return unwrap(protocol::udpSocket::bind(0)).port(); }

// An OSC input on @p port for messages to @p address (any when empty),
// active as a playing plan's is.
std::shared_ptr<primitives::oscPort> input(primitives::ioHub& io, std::uint16_t port,
                                           std::string address = {}) {
    auto in = io.openOscIn("in", port, std::move(address));
    in->setActive(true);
    return in;
}

// Lowers the process's descriptor limit to its lowest free descriptor for
// as long as it lives, so that no descriptor can be opened.
}  // namespace

TEST_CASE("the OSC bridge carries messages in and out over loopback") {
    const auto sender = support::unwrap(protocol::udpSocket::bind(0));
    const auto listener = support::unwrap(protocol::udpSocket::bind(0));
    // A free port for the bridge's input: bound, read, and closed at once.
    const auto inPort = support::unwrap(protocol::udpSocket::bind(0)).port();

    graph g;
    g.pipe(control(1));
    auto& in = g.node("in", "oscIn");
    in.params = {{"port", static_cast<double>(inPort)}, {"address", std::string("/knob")}};
    in.outputs["value"] = {control(1)};
    auto& out = g.node("out", "oscOut");
    out.params = {{"port", static_cast<double>(listener.port())}, {"address", std::string("/echo")}};
    out.inputs["value"] = {control(1)};
    renderer r;
    r.play(g.spec);
    r.run(64);

    server::oscBridge bridge;
    std::ostringstream errors;
    bridge.pump(r.engine.io(), 0.0, errors);  // opens the input socket
    CHECK(errors.str().empty());
    sender.send(protocol::encode({.address = "/knob", .args = {0.625f}}), {inPort});
    std::optional<float> echoed;
    for (int i = 0; i < 200 && echoed != 0.625f; ++i) {
        bridge.pump(r.engine.io(), 0.0, errors);
        r.run(64);
        bridge.pump(r.engine.io(), 0.0, errors);
        while (auto d = listener.receive(1)) {
            if (const auto m = protocol::decode(d->bytes); m && m->address == "/echo") echoed = m->float32(0);
        }
    }
    CHECK(echoed == 0.625f);
}

TEST_CASE("the OSC bridge reads a bounded number of datagrams per call") {
    const auto sender = support::unwrap(protocol::udpSocket::bind(0));
    // A free port for the bridge's input: bound, read, and closed at once.
    const auto inPort = support::unwrap(protocol::udpSocket::bind(0)).port();

    graph g;
    g.pipe(control(1));
    auto& in = g.node("in", "oscIn");
    in.params = {{"port", static_cast<double>(inPort)}, {"address", std::string("/knob")}};
    in.outputs["value"] = {control(1)};
    renderer r;
    r.play(g.spec);
    r.run(64);

    server::oscBridge bridge;
    std::ostringstream errors;
    CHECK(bridge.pump(r.engine.io(), 0.0, errors) == 0);  // opens the input socket
    const std::size_t sent = server::oscBridge::maxDatagramsPerPump + 44;
    const auto packet = protocol::encode({.address = "/knob", .args = {0.5f}});
    for (std::size_t i = 0; i < sent; ++i) REQUIRE(sender.send(packet, {inPort}));
    // Pump until every datagram is read, however late loopback delivers
    // them: no call reads more than the bound, and one reads exactly it.
    std::size_t read = 0;
    std::size_t largest = 0;
    CHECK(support::waitUntil([&] {
        const std::size_t now = bridge.pump(r.engine.io(), 0.0, errors);
        CHECK(now <= server::oscBridge::maxDatagramsPerPump);
        largest = std::max(largest, now);
        read += now;
        return read == sent;
    }));
    CHECK(largest == server::oscBridge::maxDatagramsPerPump);
}

TEST_CASE("the OSC bridge reports a port it cannot listen on once and opens it when it frees") {
    std::optional<protocol::udpSocket> holder = protocol::udpSocket::bind(0);
    const auto port = unwrap(holder).port();
    primitives::ioHub io;
    const auto in = input(io, port);
    server::oscBridge bridge;
    std::ostringstream errors;
    CHECK(bridge.pump(io, 0.0, errors) == 0);
    const auto reported = errors.str();
    CHECK_THAT(reported, StartsWith("espresso-server: cannot listen for OSC on 127.0.0.1:" +
                                    std::to_string(port) + ": bind 127.0.0.1: "));
    CHECK(std::ranges::count(reported, '\n') == 1);
    bridge.pump(io, 0.0, errors);
    CHECK(errors.str() == reported);  // once, not at every call

    holder.reset();  // the port frees
    bridge.pump(io, 0.0, errors);
    CHECK(errors.str() == reported);
    const auto sender = unwrap(protocol::udpSocket::bind(0));
    REQUIRE(sender.send(message(0.5f), {port}));
    CHECK(waitUntil([&] { return bridge.pump(io, 0.0, errors) == 1; }));
    primitives::oscPacket p;
    REQUIRE(in->queue.pop(p));
    CHECK(server::bytesOf(p.used()) == message(0.5f));
}

TEST_CASE("the OSC bridge drops a datagram too large for a port and delivers the next") {
    const auto port = freePort();
    primitives::ioHub io;
    const auto in = input(io, port);
    server::oscBridge bridge;
    std::ostringstream errors;
    bridge.pump(io, 0.0, errors);  // opens the input socket
    REQUIRE(errors.str().empty());
    const auto sender = unwrap(protocol::udpSocket::bind(0));
    REQUIRE(sender.send(server::oscBytes(primitives::maxOscBytes + 1, std::uint8_t{0}), {port}));
    REQUIRE(sender.send(message(0.5f), {port}));
    std::size_t read = 0;
    CHECK(waitUntil([&] {
        read += bridge.pump(io, 0.0, errors);
        return read == 2;
    }));
    CHECK(io.drops() == 1);
    primitives::oscPacket p;
    REQUIRE(in->queue.pop(p));
    CHECK(server::bytesOf(p.used()) == message(0.5f));
    CHECK_FALSE(in->queue.pop(p));
}

TEST_CASE("the OSC bridge reports an output socket it cannot open once and drops its packets") {
    const auto listener = unwrap(protocol::udpSocket::bind(0));
    primitives::ioHub io;
    const auto out = io.openOscOut(listener.port());
    server::oscBridge bridge;
    bridge.openOutputWith([](std::string& error) -> std::optional<protocol::udpSocket> {
        error = "socket: Too many open files";
        return std::nullopt;
    });
    std::ostringstream errors;
    REQUIRE(out->queue.push(packetOf(message(1.0f))));
    REQUIRE(out->queue.push(packetOf(message(2.0f))));
    bridge.pump(io, 0.0, errors);
    CHECK(errors.str() ==
          "espresso-server: cannot open a socket for OSC output: socket: Too many open files\n");
    CHECK(io.drops() == 2);  // once for both packets
    // Once a socket opens, the next packet is sent.
    bridge.openOutputWith([](std::string& error) { return protocol::udpSocket::bind(0, error); });
    REQUIRE(out->queue.push(packetOf(message(3.0f))));
    bridge.pump(io, 0.0, errors);
    const auto d = listener.receive(replyTimeoutMs);
    REQUIRE(d);
    CHECK(unwrap(d).bytes == message(3.0f));
    CHECK(io.drops() == 2);
}

TEST_CASE("the page listener follows the page port of each call (io.md 12.2)") {
    primitives::ioHub io;
    scriptedPages pages;
    server::oscBridge bridge(pages);
    std::ostringstream errors;
    bridge.pump(io, 0.0, errors, std::nullopt, 8000);
    bridge.pump(io, 0.0, errors, std::nullopt, std::nullopt);
    bridge.pump(io, 0.0, errors, std::nullopt, 8001);
    CHECK(pages.listened == std::vector<std::optional<std::uint16_t>>{8000, std::nullopt, 8001});
    CHECK(errors.str().empty());
}

TEST_CASE("pages' messages reach the page port's OSC inputs in order at the call's time (io.md 12.3)") {
    const auto port = freePort();
    primitives::ioHub io;
    const auto in = input(io, port);
    scriptedPages pages;
    pages.waiting = {message(0.25f), message(0.5f, "/other")};
    server::oscBridge bridge(pages);
    std::ostringstream errors;
    bridge.pump(io, 12.5, errors, std::nullopt, port);
    CHECK(pages.waiting.empty());
    primitives::oscPacket p;
    REQUIRE(in->queue.pop(p));
    CHECK(p.time == 12.5);
    CHECK(server::bytesOf(p.used()) == message(0.25f));
    REQUIRE(in->queue.pop(p));
    CHECK(server::bytesOf(p.used()) == message(0.5f, "/other"));
    CHECK_FALSE(in->queue.pop(p));
    CHECK(io.drops() == 0);
}

TEST_CASE("a page message that no node reads is a drop (io.md 12.3)") {
    const auto port = freePort();
    primitives::ioHub io;
    const auto in = input(io, port, "/knob");
    scriptedPages pages;
    server::oscBridge bridge(pages);
    std::ostringstream errors;
    primitives::oscPacket p;
    pages.waiting = {message(0.25f, "/other")};
    bridge.pump(io, 0.0, errors, std::nullopt, port);
    CHECK(io.drops() == 1);
    // With no page port, no node reads pages.
    pages.waiting = {message(0.25f)};
    bridge.pump(io, 0.0, errors, std::nullopt, std::nullopt);
    CHECK(io.drops() == 2);
    // Nor a message larger than a port holds.
    pages.waiting = {server::oscBytes(primitives::maxOscBytes + 1, std::uint8_t{0})};
    bridge.pump(io, 0.0, errors, std::nullopt, port);
    CHECK(io.drops() == 3);
    CHECK_FALSE(in->queue.pop(p));
    pages.waiting = {message(0.75f)};
    bridge.pump(io, 0.0, errors, std::nullopt, port);
    CHECK(io.drops() == 3);
    REQUIRE(in->queue.pop(p));
    CHECK(server::bytesOf(p.used()) == message(0.75f));
}

TEST_CASE("output to the page port goes to pages and never to UDP (io.md 12.3)") {
    // A UDP peer on the page port, as a controller or the server's own
    // input socket would be.
    const auto peer = unwrap(protocol::udpSocket::bind(0));
    const auto port = peer.port();
    primitives::ioHub io;
    const auto out = io.openOscOut(port);
    scriptedPages pages;
    server::oscBridge bridge(pages);
    std::ostringstream errors;
    std::ostringstream logText;
    std::ostream& log = logText;
    REQUIRE(out->queue.push(packetOf(message(1.0f))));
    bridge.pump(io, 0.0, errors, std::ref(log), port);
    CHECK(pages.sent == std::vector<server::oscBytes>{message(1.0f)});
    CHECK(logText.str().empty());  // nothing went over UDP
    CHECK(io.drops() == 0);
    // Once the port is no longer the page port, its output goes over UDP:
    // the peer's first datagram is the second packet, so the first never
    // reached it.
    REQUIRE(out->queue.push(packetOf(message(2.0f))));
    bridge.pump(io, 0.0, errors, std::ref(log), std::nullopt);
    const auto d = peer.receive(replyTimeoutMs);
    REQUIRE(d);
    CHECK(unwrap(d).bytes == message(2.0f));
    CHECK(pages.sent.size() == 1);
    CHECK(logText.str() ==
          "osc -> :" + std::to_string(port) + " (" + std::to_string(message(2.0f).size()) + " bytes)\n");
    CHECK(errors.str().empty());
}

TEST_CASE("a piece's output to the page port reaches pages and is not dropped (io.md 12.3)") {
    graph g;
    g.pipe(events(0)).pipe(control(1));
    auto& lfo = g.node("lfo", "lfo");
    lfo.params = {{"depth", 0.0}, {"offset", 0.5}};
    lfo.outputs["out"] = {control(1)};
    auto& out = g.node("send", "oscOut");
    out.params = {{"port", 8123.0}, {"address", std::string("/level")}};
    out.inputs["value"] = {control(1)};
    renderer r;
    r.play(g.spec);
    r.run(64);
    scriptedPages pages;
    server::oscBridge bridge(pages);
    std::ostringstream errors;
    bridge.pump(r.engine.io(), 0.0, errors, std::nullopt, 8123);
    REQUIRE(pages.sent.size() == 1);
    CHECK(protocol::decode(pages.sent.front()) == protocol::message{.address = "/level", .args = {0.5f}});
    CHECK(r.engine.io().drops() == 0);
}

TEST_CASE("output to the page port of a bridge without pages is a drop") {
    const auto peer = unwrap(protocol::udpSocket::bind(0));
    primitives::ioHub io;
    const auto out = io.openOscOut(peer.port());
    server::oscBridge bridge;
    std::ostringstream errors;
    REQUIRE(out->queue.push(packetOf(message(1.0f))));
    bridge.pump(io, 0.0, errors, std::nullopt, peer.port());
    CHECK(io.drops() == 1);
    CHECK_FALSE(peer.receive(0));
    CHECK(errors.str().empty());
}
