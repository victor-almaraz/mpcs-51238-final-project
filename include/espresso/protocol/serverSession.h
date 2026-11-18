/// @file
/// Server side of the protocol: connection state, request validation,
/// reply caching, and IR assembly (`protocol.md` §3–§6). The session has
/// no sockets and no threads; the network thread feeds it messages and
/// sends what it returns, usually through a serverLink.

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/protocol/irTransfer.h>
#include <espresso/protocol/osc.h>
#include <espresso/protocol/udp.h>

namespace protocol {

/// Values reported in `/hello.ok`.
struct serverInfo {
    /// Server name and version.
    std::string serverVersion;
    /// Version of the primitive parameter tables (`protocol.md` §4.2).
    std::string metadataVersion;
    /// Audio sample rate, in hertz.
    std::int32_t sampleRate = 48000;
    /// Audio block size, in samples.
    std::int32_t blockSize = 256;
};

/// Values reported in `/status.reply` and `/counters`.
struct serverCounters {
    /// Audio thread load, from 0 to 1.
    float load = 0.0f;
    /// Blocks that missed their deadline.
    std::int32_t missedDeadlines = 0;
    /// Device underruns and overruns.
    std::int32_t xruns = 0;
    /// Requests dropped by full queues.
    std::int32_t drops = 0;
    /// Ring buffer overflows between clock domains.
    std::int32_t overflows = 0;
    /// The active plan, or 0.
    std::int32_t activePlan = 0;
    /// Tempo of the beat clock, in beats per minute.
    float tempo = 120.0f;
    /// Position of the beat clock, in beats since the engine started.
    double beat = 0.0;
    /// Beats per bar of the active plan.
    std::int32_t meter = 4;
};

/// What the session drives. The server's engine host implements it.
class engine {
public:
    engine() = default;
    virtual ~engine() = default;
    // An interface: implementations are used by reference, never copied.
    engine(const engine&) = delete;
    engine& operator=(const engine&) = delete;
    engine(engine&&) = delete;
    engine& operator=(engine&&) = delete;
    /// Accepts assembled IR for @p planId. Returns an error message when
    /// the IR is invalid (`protocol.md` §6.4).
    virtual std::optional<std::string> loadPlan(std::int32_t planId, std::string ir) = 0;
    /// True when plan @p planId is loaded and not yet played.
    virtual bool hasPlan(std::int32_t planId) const = 0;
    /// Schedules a swap to @p planId, which it consumes: a second `/play`
    /// of the same plan fails with `unknownPlan`. @p fade is a validFade()
    /// in seconds; @p quant is `none`, `beat`, or `bar`.
    virtual void play(std::int32_t planId, float fade, std::string_view quant) = 0;
    /// Fades out the active plan over @p fade seconds, a validFade().
    virtual void stop(float fade) = 0;
    /// Sets control pipe @p index to @p value.
    virtual void setControl(std::int32_t index, float value) = 0;
    /// The value of control pipe @p index.
    virtual float control(std::int32_t index) const = 0;
    /// The current counters.
    virtual serverCounters counters() const = 0;
    /// Asks the engine to shut down.
    virtual void quit() = 0;
};

/// A message and its destination.
struct outgoing {
    /// Destination client.
    endpoint to;
    /// The message to send.
    message msg;
};

/// Validates and answers client messages for one server (`protocol.md` §3–§6).
///
/// The session remembers each client that completed `/hello`. UDP gives no
/// notice when a client goes away, and an idle editor sends nothing while
/// it still receives notifications, so clients are not expired by time.
/// Instead, a protocol::client sends `/notify 0` when it is destroyed, and
/// at most `maxClients` clients are kept: a `/hello` from a new client
/// beyond that forgets the client heard from least recently.
class serverSession {
public:
    /// Creates a session that reports @p info and drives @p e.
    serverSession(serverInfo info, engine& e);

    /// Decodes and handles one datagram. Undecodable datagrams are
    /// dropped and counted (`protocol.md` §2.4).
    std::vector<outgoing> handleDatagram(std::span<const std::uint8_t> bytes, endpoint from);

    /// Handles one decoded message from @p from.
    std::vector<outgoing> handle(const message& m, endpoint from);

    /// @p m addressed to every client that sent `/notify 1`.
    std::vector<outgoing> broadcast(const message& m) const;

    /// The periodic `/counters` message for subscribed clients: missed
    /// deadlines, xruns, drops, overflows, load, tempo, beat, and meter.
    std::vector<outgoing> countersMessages() const;

    /// Counts @p n datagrams that did not decode or could not be sent
    /// (`protocol.md` §2.4). They are reported in the drops counter.
    void countDropped(std::size_t n = 1) noexcept { dropped_ += n; }

    /// Datagrams dropped because they did not decode or could not be
    /// sent, and IR transfers abandoned to stay within the limits.
    std::size_t droppedDatagrams() const noexcept { return dropped_ + ir_.abandoned(); }

    /// The engine's counters, with the session's drops added to `drops`.
    serverCounters counters() const;

    /// True when @p client completed `/hello`.
    bool isConnected(endpoint client) const { return clients_.contains(client); }

    /// Number of clients that completed `/hello` and are remembered.
    std::size_t clientCount() const noexcept { return clients_.size(); }

private:
    struct clientState {
        bool notify = false;
        // When the client was last heard from, in messages handled.
        std::uint64_t lastHeard = 0;
        // Recent request IDs and the replies sent for them (§3.3).
        std::deque<std::pair<std::int32_t, std::vector<message>>> recent;
    };

    // One handler per address, a member function invoked on this session.
    // Each returns the replies to send; the request ID is 0 for messages
    // that carry none.
    using handler =
        std::function<std::vector<message>(serverSession&, const message&, endpoint, std::int32_t)>;
    struct route {
        std::string_view address;
        bool hasRequestId;
        handler handle;
    };
    static std::optional<std::reference_wrapper<const route>> findRoute(std::string_view address);

    std::vector<message> onHello(const message& m, endpoint from, std::int32_t id);
    std::vector<message> onNotify(const message& m, endpoint from, std::int32_t id);
    std::vector<message> onStatus(const message& m, endpoint from, std::int32_t id);
    std::vector<message> onIrBegin(const message& m, endpoint from, std::int32_t id);
    std::vector<message> onIrChunk(const message& m, endpoint from, std::int32_t id);
    std::vector<message> onIrEnd(const message& m, endpoint from, std::int32_t id);
    std::vector<message> onPlay(const message& m, endpoint from, std::int32_t id);
    std::vector<message> onStop(const message& m, endpoint from, std::int32_t id);
    std::vector<message> onControlSet(const message& m, endpoint from, std::int32_t id);
    std::vector<message> onControlGet(const message& m, endpoint from, std::int32_t id);
    std::vector<message> onSync(const message& m, endpoint from, std::int32_t id);
    std::vector<message> onQuit(const message& m, endpoint from, std::int32_t id);

    serverInfo info_;
    engine& engine_;
    std::map<endpoint, clientState> clients_;
    std::uint64_t heard_ = 0;
    irAssembler ir_;
    std::size_t dropped_ = 0;
};

/// Builds a `/fail` reply.
message failure(std::int32_t requestId, errorCode code, std::string text);

/// What a server's network loop reports: each message received (`<-`) and
/// sent (`->`), and each datagram dropped because it did not decode.
/// Either function may be empty.
struct trafficLog {
    /// Called with the direction, the peer, and the message.
    std::function<void(std::string_view direction, endpoint peer, const message& m)> onMessage;
    /// Called with the sender of an undecodable datagram.
    std::function<void(endpoint peer)> onMalformed;
};

/// Sends each of @p out on @p socket and logs it on @p log. Returns the
/// number of messages that could not be sent.
std::size_t sendAll(const udpSocket& socket, std::span<const outgoing> out, const trafficLog& log = {});

/// The network side of a server: receives datagrams on a socket, passes
/// them to a session, and sends replies, notifications, and `/counters`.
/// Failed sends and undecodable datagrams are counted in the session's
/// drops (`protocol.md` §2.4, §5).
class serverLink {
public:
    /// Serves @p session on @p socket; both must outlive the link.
    serverLink(const udpSocket& socket, serverSession& session, trafficLog log = {});

    /// Waits up to @p timeoutMs for one datagram, decodes it once, handles
    /// it, and sends the replies. Returns true when a datagram arrived.
    bool serveOnce(int timeoutMs);

    /// Sends @p m to every client that sent `/notify 1`.
    void broadcast(const message& m);

    /// Sends `/counters` when `countersIntervalMs` passed since the last
    /// time it did, as of @p now.
    void sendCountersIfDue(std::chrono::steady_clock::time_point now);

private:
    void send(const std::vector<outgoing>& out);

    const udpSocket& socket_;
    serverSession& session_;
    trafficLog log_;
    std::chrono::steady_clock::time_point nextCounters_;
};

}  // namespace protocol
