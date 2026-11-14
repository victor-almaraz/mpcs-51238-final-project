/// @file
/// Client side of the protocol: request IDs, timeouts and retries, IR
/// transfer, and delivery of unsolicited server messages
/// (`protocol.md` §3, §5, §6).

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <proj/protocol/messages.h>
#include <proj/protocol/jthread.h>
#include <proj/protocol/osc.h>
#include <proj/protocol/udp.h>

namespace protocol {

/// How a client reaches the server and how long it waits for replies.
struct clientOptions {
    /// UDP port of the server on 127.0.0.1.
    std::uint16_t serverPort = defaultServerPort;
    /// Time to wait for each reply, in milliseconds.
    int timeoutMs = defaultTimeoutMs;
    /// Resends after the first attempt.
    int retries = defaultRetries;
};

/// Result of a request: the reply, or why there is none.
struct requestResult {
    /// The reply, if one arrived.
    std::optional<message> reply;
    /// Why no reply arrived, when reply is empty.
    std::string error;

    /// True for a reply other than `/fail`.
    bool ok() const { return reply && reply->address != address::fail; }
    /// The `/fail` code, if the reply is a failure.
    std::optional<errorCode> failCode() const;
    /// The `/fail` text, the error, or empty.
    std::string failText() const;
};

/// A client endpoint. request() blocks and may be called from one thread
/// at a time; a receiver thread routes replies and passes unsolicited
/// messages to the listener.
class client {
public:
    /// Receives server messages that are not replies to a request.
    using listener = std::function<void(const message&)>;

    /// Creates a client for @p options. @p unsolicited receives notifications and
    /// unsolicited failures, on the receiver thread.
    explicit client(clientOptions options = {}, listener unsolicited = {});
    ~client();

    client(const client&) = delete;
    client& operator=(const client&) = delete;

    /// Binds a local socket and starts the receiver thread. Sets @p error
    /// and returns false on failure.
    bool open(std::string& error);

    /// Sends @p addr with a fresh request ID followed by @p args, and
    /// waits for the reply, retrying after each timeout (§3.2).
    requestResult request(std::string_view addr, std::vector<argument> args = {});

    /// Sends a message that expects no reply.
    bool send(const message& m);

    /// Transfers @p payload as plan @p planId: `/ir.begin`, chunks, and
    /// `/ir.end`, resending missing chunks (§6). Returns an error
    /// message on failure.
    std::optional<std::string> submitIr(std::int32_t planId, std::string_view payload);

    /// Time of the last datagram from the server.
    std::chrono::steady_clock::time_point lastHeard() const;

private:
    void receiveLoop(const proj::stop_token& stop);

    clientOptions options_;
    listener unsolicited_;
    std::optional<udpSocket> socket_;

    mutable std::mutex mutex_;
    std::condition_variable replied_;
    std::map<std::int32_t, std::optional<message>> waiting_;
    std::int32_t nextId_ = 1;
    std::chrono::steady_clock::time_point lastHeard_{};

    // Last, so it stops and joins before the members it uses are destroyed.
    proj::jthread receiver_;
};

} // namespace protocol
