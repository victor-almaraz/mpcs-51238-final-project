// The web bridge's transport (io.md §3.3): HTTP requests, streamed
// responses, and WebSockets behind an interface, so the bridge's rules
// are tested against a scripted transport. The server's is libcurl.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace server {

/// An HTTP request.
struct httpRequest {
    /// `GET`, `POST`, or `PUT`.
    std::string method = "GET";
    /// `http://` or `https://` URL.
    std::string url;
    /// Headers, each `Name: value`.
    std::vector<std::string> headers;
    /// The body, for POST and PUT.
    std::string body;
    /// The longest the whole transfer may take; zero for a stream, which
    /// has none.
    std::chrono::milliseconds timeout{0};
};

/// Moves HTTP requests and WebSocket messages. One thread uses a
/// transport: it starts transfers, and wait() calls their handlers on
/// that thread. After stop() a transfer calls no handler.
class httpTransport {
public:
    /// Names a transfer or a socket.
    using id = std::uint64_t;

    /// What a request reports.
    struct handlers {
        /// A piece of the response body, with the response's status.
        std::function<void(long status, std::string_view chunk)> data;
        /// The transfer ended: its status, or 0 with what went wrong when it
        /// failed before one arrived or was cut off.
        std::function<void(long status, const std::string& error)> done;
    };

    /// What a WebSocket reports.
    struct socketHandlers {
        /// The connection opened.
        std::function<void()> opened;
        /// A whole message: text, or binary when @p binary.
        std::function<void(std::string_view message, bool binary)> message;
        /// The connection closed or failed, and why.
        std::function<void(const std::string& reason)> closed;
    };

    virtual ~httpTransport() = default;

    /// Starts @p request.
    virtual id start(const httpRequest& request, handlers events) = 0;
    /// Opens a WebSocket to @p url (`ws://` or `wss://`) with @p headers.
    virtual id openSocket(const std::string& url, const std::vector<std::string>& headers,
                          socketHandlers events) = 0;
    /// Sends a text message on an open socket.
    virtual void sendText(id socket, std::string_view text) = 0;
    /// Cancels a transfer or closes a socket.
    virtual void stop(id transfer) = 0;
    /// Moves data for at most @p most, calling handlers as transfers
    /// progress, and returns sooner when one does.
    virtual void wait(std::chrono::milliseconds most) = 0;

protected:
    httpTransport() = default;
    httpTransport(const httpTransport&) = default;
    httpTransport& operator=(const httpTransport&) = default;
    httpTransport(httpTransport&&) = default;
    httpTransport& operator=(httpTransport&&) = default;
};

}  // namespace server
