/// @file
/// UDP sockets on the loopback interface, on POSIX sockets
/// (`protocol.md` §1.2, §2.3).

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace protocol {

/// A UDP peer on 127.0.0.1.
struct endpoint {
    std::uint16_t port = 0;
    friend auto operator<=>(const endpoint&, const endpoint&) = default;
};

/// One received datagram.
struct datagram {
    endpoint from;
    std::vector<std::uint8_t> bytes;
};

/// A UDP socket bound to 127.0.0.1. Movable, not copyable.
class udpSocket {
public:
    /// Binds to 127.0.0.1:@p port (0 picks a free port). Returns nothing
    /// and sets @p error on failure.
    static std::optional<udpSocket> bind(std::uint16_t port, std::string* error = nullptr);

    udpSocket(udpSocket&& other) noexcept;
    udpSocket& operator=(udpSocket&& other) noexcept;
    udpSocket(const udpSocket&) = delete;
    udpSocket& operator=(const udpSocket&) = delete;
    ~udpSocket();

    /// The bound port.
    std::uint16_t port() const noexcept { return port_; }

    /// Sends one datagram. Returns false on failure.
    bool send(std::span<const std::uint8_t> bytes, endpoint to) const;

    /// Waits up to @p timeoutMs for a datagram.
    std::optional<datagram> receive(int timeoutMs) const;

private:
    udpSocket(int fd, std::uint16_t port) noexcept : fd_(fd), port_(port) {}
    int fd_ = -1;
    std::uint16_t port_ = 0;
};

} // namespace protocol
