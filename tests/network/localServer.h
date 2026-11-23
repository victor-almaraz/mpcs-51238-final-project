// A local HTTP and WebSocket server on 127.0.0.1 for the libcurl
// transport's test (io.md §14.4): a JSON document, an event stream, a
// WebSocket that answers a hello, and an endpoint that records POST
// bodies. POSIX sockets, one thread per connection; it makes no external
// connection.

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <espresso/protocol/jthread.h>
#include <espresso/protocol/udp.h>

#include "webSocket.h"

#include <poll.h>

namespace support {

class localServer {
public:
    /// The document GET /now serves.
    static constexpr std::string_view now = R"({"main": {"temp": 21.5}})";
    /// The message GET /stream sends, then holds the stream open.
    static constexpr std::string_view event = "event: edit\ndata: {\"n\": 7}\n\n";
    /// The message /feed sends after it receives a text message.
    static constexpr std::string_view feed = R"({"length": 140})";

    localServer() : listener_(::socket(AF_INET, SOCK_STREAM, 0)) {
        REQUIRE(listener_ >= 0);
        int on = 1;
        REQUIRE(::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) == 0);
        sockaddr_in a = protocol::loopback(0);
        REQUIRE(::bind(listener_, protocol::asSockaddr(a), sizeof a) == 0);
        REQUIRE(::listen(listener_, 16) == 0);
        socklen_t length = sizeof a;
        REQUIRE(::getsockname(listener_, protocol::asSockaddr(a), &length) == 0);
        port_ = ntohs(a.sin_port);
        accepter_ =
            std::make_unique<espresso::jthread>([this](const espresso::stop_token& stop) { accept(stop); });
    }

    ~localServer() {
        accepter_.reset();     // no connection is added after this
        connections_.clear();  // each stops and joins
        ::close(listener_);
    }

    localServer(const localServer&) = delete;
    localServer& operator=(const localServer&) = delete;
    localServer(localServer&&) = delete;
    localServer& operator=(localServer&&) = delete;

    std::string url(std::string_view path, std::string_view scheme = "http") const {
        return std::string(scheme) + "://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }

    /// POST bodies received, and text messages received on /feed.
    std::vector<std::string> posted() const {
        const std::scoped_lock lock(mutex_);
        return posted_;
    }
    std::vector<std::string> hellos() const {
        const std::scoped_lock lock(mutex_);
        return hellos_;
    }
    /// Requests seen, as `METHOD path`.
    std::vector<std::string> requests() const {
        const std::scoped_lock lock(mutex_);
        return requests_;
    }

private:
    void accept(const espresso::stop_token& stop) {
        while (!stop.stop_requested()) {
            pollfd p{.fd = listener_, .events = POLLIN, .revents = 0};
            if (::poll(&p, 1, 20) <= 0) continue;
            const int fd = ::accept(listener_, nullptr, nullptr);
            if (fd < 0) continue;
            const std::scoped_lock lock(mutex_);
            connections_.push_back(std::make_unique<espresso::jthread>(
                [this, fd](const espresso::stop_token& s) { serve(fd, s); }));
        }
    }

    // Reads into @p buffer until it holds @p until, or the connection
    // closes or stops; false then.
    static bool readUntil(int fd, std::string& buffer, std::string_view until,
                          const espresso::stop_token& stop) {
        while (buffer.find(until) == std::string::npos) {
            if (!readSome(fd, buffer, stop)) return false;
        }
        return true;
    }
    static bool readSome(int fd, std::string& buffer, const espresso::stop_token& stop) {
        while (!stop.stop_requested()) {
            pollfd p{.fd = fd, .events = POLLIN, .revents = 0};
            if (::poll(&p, 1, 20) <= 0) continue;
            std::array<char, 4096> chunk{};
            const auto n = ::read(fd, chunk.data(), chunk.size());
            if (n <= 0) return false;
            buffer.append(chunk.data(), static_cast<std::size_t>(n));
            return true;
        }
        return false;
    }
    static void send(int fd, std::string_view bytes) {
        while (!bytes.empty()) {
            const auto n = ::send(fd, bytes.data(), bytes.size(), 0);
            if (n <= 0) return;
            bytes.remove_prefix(static_cast<std::size_t>(n));
        }
    }
    static std::string header(std::string_view head, std::string_view name) {
        const auto at = head.find(std::string(name) + ": ");
        if (at == std::string_view::npos) return {};
        const auto start = at + name.size() + 2;
        return std::string(head.substr(start, head.find("\r\n", start) - start));
    }
    void hold(int fd, const espresso::stop_token& stop) {
        std::string ignored;
        while (readSome(fd, ignored, stop)) ignored.clear();
    }

    void serve(int fd, const espresso::stop_token& stop) {
        std::string buffer;
        if (readUntil(fd, buffer, "\r\n\r\n", stop)) {
            const auto end = buffer.find("\r\n\r\n");
            const std::string head = buffer.substr(0, end);
            std::string body = buffer.substr(end + 4);
            const std::string line = head.substr(0, head.find("\r\n"));
            const std::string method = line.substr(0, line.find(' '));
            const std::string path = line.substr(line.find(' ') + 1, line.rfind(' ') - line.find(' ') - 1);
            {
                const std::scoped_lock lock(mutex_);
                requests_.push_back(method + " " + path);
            }
            if (path == "/now") {
                send(fd, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                             std::to_string(now.size()) + "\r\nConnection: close\r\n\r\n" + std::string(now));
            } else if (path == "/stream") {
                send(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n" +
                             std::string(event));
                hold(fd, stop);
            } else if (method == "POST" && path == "/level") {
                const auto length = static_cast<std::size_t>(std::stoul(header(head, "Content-Length")));
                while (body.size() < length && readSome(fd, body, stop)) {
                }
                {
                    const std::scoped_lock lock(mutex_);
                    posted_.push_back(body.substr(0, length));
                }
                send(fd, "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
            } else if (path == "/feed") {
                send(fd, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         "Sec-WebSocket-Accept: " +
                             server::acceptKey(header(head, "Sec-WebSocket-Key")) + "\r\n\r\n");
                server::frameReader frames(true, std::size_t{1} << 20U);
                bool answered = false;
                std::string bytes = std::move(body);
                do {
                    frames.feed(std::exchange(bytes, {}), [&](const server::frameMessage& m) {
                        if (m.op != server::opcode::text) return;
                        {
                            const std::scoped_lock lock(mutex_);
                            hellos_.push_back(m.payload);
                        }
                        if (!answered) send(fd, server::encodeFrame(server::opcode::text, feed));
                        answered = true;
                    });
                } while (readSome(fd, bytes, stop));
            } else {
                send(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            }
        }
        ::close(fd);
    }

    int listener_ = -1;
    std::uint16_t port_ = 0;
    mutable std::mutex mutex_;
    std::vector<std::string> posted_;
    std::vector<std::string> hellos_;
    std::vector<std::string> requests_;
    std::vector<std::unique_ptr<espresso::jthread>> connections_;
    std::unique_ptr<espresso::jthread> accepter_;
};

}  // namespace support
