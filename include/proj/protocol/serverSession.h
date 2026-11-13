/// @file
/// Server side of the protocol: connection state, request validation,
/// reply caching, and IR assembly (`protocol.md` §3–§6). The session has
/// no sockets and no threads; the network thread feeds it messages and
/// sends what it returns.

#pragma once

#include <cstdint>
#include <deque>
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
    std::string serverVersion;
    std::string metadataVersion;
    std::int32_t sampleRate = 48000;
    std::int32_t blockSize = 256;
};

/// Values reported in `/status.reply` and `/counters`.
struct serverCounters {
    float load = 0.0f;
    std::int32_t missedDeadlines = 0;
    std::int32_t xruns = 0;
    std::int32_t drops = 0;
    std::int32_t overflows = 0;
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
    virtual bool hasPlan(std::int32_t planId) const = 0;
    /// Schedules a swap to @p planId. @p quant is `none`, `beat`, or `bar`.
    virtual void play(std::int32_t planId, float fade, std::string_view quant) = 0;
    virtual void stop(float fade) = 0;
    virtual void setControl(std::int32_t index, float value) = 0;
    virtual float control(std::int32_t index) const = 0;
    virtual serverCounters counters() const = 0;
    virtual void quit() = 0;
};

/// A message and its destination.
struct outgoing {
    endpoint to;
    message msg;
};

class serverSession {
public:
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

    std::vector<message> dispatch(const message& m, endpoint from, std::int32_t requestId);
    std::vector<message> handleIrEnd(std::int32_t requestId, std::int32_t planId);

    serverInfo info_;
    engine& engine_;
    std::map<endpoint, clientState> clients_;
    irAssembler ir_;
    std::size_t dropped_ = 0;
};

/// Builds a `/fail` reply.
message failure(std::int32_t requestId, errorCode code, std::string text);

} // namespace protocol
