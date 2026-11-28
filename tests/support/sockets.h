// TCP sockets on POSIX for the tests of the page bridge and the web
// transport (io.md §14.1, §14.4): a client connection, written and read as
// raw HTTP with a bounded wait, and the reads and writes of a test's own
// server.

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <espresso/protocol/udp.h>

#include "fileHandle.h"
#include "support/timing.h"

#include <poll.h>

namespace support {

/// What receiveSome() found.
enum class received : std::uint8_t {
    data,    ///< Bytes arrived.
    idle,    ///< Nothing arrived within the wait.
    closed,  ///< The peer closed the connection, or the read failed.
};

/// Waits up to @p waitMs for bytes on @p fd and appends what one read
/// takes to @p buffer.
inline received receiveSome(int fd, std::string& buffer, int waitMs = 20) {
    pollfd p{.fd = fd, .events = POLLIN, .revents = 0};
    if (::poll(&p, 1, waitMs) <= 0) return received::idle;
    std::array<char, 8192> chunk{};
    const auto n = ::read(fd, chunk.data(), chunk.size());
    if (n <= 0) return received::closed;
    buffer.append(chunk.data(), static_cast<std::size_t>(n));
    return received::data;
}

/// Sends all of @p bytes on @p fd; false when the connection fails first.
inline bool sendAll(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto n = ::send(fd, bytes.data(), bytes.size(), 0);
        if (n <= 0) return false;
        bytes.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

/// A TCP connection to an HTTP server, written and read as raw bytes.
class httpClient {
public:
    /// Connects to 127.0.0.1:@p port; connected() says whether it did.
    explicit httpClient(std::uint16_t port) : httpClient(protocol::loopback(port)) {}
    /// Connects to @p address, an IPv4 address in text, at @p port.
    httpClient(const std::string& address, std::uint16_t port) : httpClient(addressOf(address, port)) {}

    bool connected() const noexcept { return connected_; }

    void write(std::string_view bytes) const { REQUIRE(sendAll(fd_.get(), bytes)); }
    // Says it will send no more (a half-close); it still reads.
    void finish() const { ::shutdown(fd_.get(), SHUT_WR); }

    // Reads until @p done holds for what has arrived, the peer closes, or
    // the wait times out.
    template <typename Done>
    std::string readUntil(Done done) {
        const auto deadline = std::chrono::steady_clock::now() + settleTimeout;
        while (!done(buffer_) && std::chrono::steady_clock::now() < deadline) {
            if (receiveSome(fd_.get(), buffer_) == received::closed) {
                closed_ = true;
                break;
            }
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
    explicit httpClient(const sockaddr_in& a) : fd_(::socket(AF_INET, SOCK_STREAM, 0)) {
        REQUIRE(fd_);
        connected_ = ::connect(fd_.get(), protocol::asSockaddr(a), sizeof a) == 0;
    }

    static sockaddr_in addressOf(const std::string& address, std::uint16_t port) {
        sockaddr_in a = protocol::loopback(port);
        REQUIRE(::inet_pton(AF_INET, address.c_str(), &a.sin_addr) == 1);
        return a;
    }

    server::fileHandle fd_;
    bool connected_ = false;
    bool closed_ = false;
    std::string buffer_;
};

}  // namespace support
