/// @file
/// Server side of the protocol: connection state, request validation,
/// reply caching, and IR assembly (`protocol.md` §3–§6). The session has
/// no sockets and no threads; the network thread feeds it messages and
/// sends what it returns.

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <proj/protocol/irTransfer.h>
#include <proj/protocol/osc.h>
#include <proj/protocol/udp.h>

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
};

/// What the session drives. The audio engine implements it; so does the
/// stub server.
class engine {
public:
    virtual ~engine() = default;
    /// Accepts assembled IR for @p planId. Returns an error message when
    /// the IR is invalid (`protocol.md` §6.4).
    virtual std::optional<std::string> loadPlan(std::int32_t planId, std::string ir) = 0;
    /// True when plan @p planId is loaded.
    virtual bool hasPlan(std::int32_t planId) const = 0;
    /// Schedules a swap to @p planId. @p quant is `none`, `beat`, or `bar`.
    virtual void play(std::int32_t planId, float fade, std::string_view quant) = 0;
    /// Fades out the active plan over @p fade seconds.
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

    /// The periodic `/counters` message for subscribed clients.
    std::vector<outgoing> countersMessages() const;

    /// Datagrams dropped because they did not decode.
    std::size_t droppedDatagrams() const noexcept { return dropped_; }

    /// True when @p client completed `/hello`.
    bool isConnected(endpoint client) const { return clients_.count(client) != 0; }

private:
    struct clientState {
        bool notify = false;
        // Recent request IDs and the replies sent for them (§3.3).
        std::deque<std::pair<std::int32_t, std::vector<message>>> recent;
    };

    // One handler per address, a member function invoked on this session.
    // Each returns the replies to send; the request ID is 0 for messages
    // that carry none.
    using handler = std::function<std::vector<message>(serverSession&, const message&, endpoint, std::int32_t)>;
    struct route {
        std::string_view address;
        bool hasRequestId;
        handler handle;
    };
    static std::optional<route> findRoute(std::string_view address);

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
    irAssembler ir_;
    std::size_t dropped_ = 0;
};

/// Builds a `/fail` reply.
message failure(std::int32_t requestId, errorCode code, std::string text);

} // namespace protocol
