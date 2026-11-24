/// @file
/// UDP sockets on the loopback interface, on POSIX sockets
/// (`protocol.md` §1.2, §2.3).

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>

namespace protocol {

/// The IPv4 address 127.0.0.1:@p port, as the POSIX socket calls take it.
sockaddr_in loopback(std::uint16_t port) noexcept;

/// @p addr as the generic address type that the POSIX socket calls take by
/// pointer: the narrow unsafe use at this boundary, confined to the two
/// overloads (modern-cpp skill, section 9). Pass the pointer straight to
/// the call; never store it.
sockaddr* asSockaddr(sockaddr_in& addr) noexcept;

/// @p addr as the generic address type, read-only, as asSockaddr(sockaddr_in&).
const sockaddr* asSockaddr(const sockaddr_in& addr) noexcept;

/// A UDP peer on 127.0.0.1.
struct endpoint {
    /// UDP port.
    std::uint16_t port = 0;
    /// Equal when the ports are equal.
    friend bool operator==(const endpoint&, const endpoint&) = default;
    /// Ordered by port, so endpoints can key maps.
    friend bool operator<(const endpoint& a, const endpoint& b) noexcept { return a.port < b.port; }
};

/// One received datagram.
struct datagram {
    /// Sender.
    endpoint from;
    /// Payload.
    std::vector<std::uint8_t> bytes;
};

/// A UDP socket bound to 127.0.0.1. Movable, not copyable.
class udpSocket {
public:
    /// Binds to 127.0.0.1:@p port (0 picks a free port). Returns nothing
    /// and sets @p error on failure.
    static std::optional<udpSocket> bind(std::uint16_t port, std::string& error);
    /// Binds to 127.0.0.1:@p port, discarding the error message.
    static std::optional<udpSocket> bind(std::uint16_t port);

    /// Takes over the socket of @p other.
    udpSocket(udpSocket&& other) noexcept;
    /// Closes this socket and takes over the socket of @p other.
    udpSocket& operator=(udpSocket&& other) noexcept;
    udpSocket(const udpSocket&) = delete;
    udpSocket& operator=(const udpSocket&) = delete;
    ~udpSocket();

    /// The bound port.
    std::uint16_t port() const noexcept { return port_; }

    /// Socket buffer size that bind() asks for, for each direction.
    static constexpr std::size_t requestedBufferBytes = std::size_t{4} * 1024 * 1024;

    /// The receive buffer size the system granted, as it reports it (Linux
    /// reports twice the usable size and caps the request at
    /// `net.core.rmem_max`); 0 when unknown.
    std::size_t receiveBufferBytes() const noexcept { return receiveBuffer_; }

    /// Sends one datagram. Returns false on failure.
    bool send(std::span<const std::uint8_t> bytes, endpoint to) const;
    /// Sends one datagram of raw bytes. Returns false on failure.
    bool send(std::span<const std::byte> bytes, endpoint to) const;

    /// Waits up to @p timeoutMs for a datagram.
    std::optional<datagram> receive(int timeoutMs) const;

private:
    udpSocket(int fd, std::uint16_t port) noexcept : fd_(fd), port_(port) {}
    int fd_ = -1;
    std::uint16_t port_ = 0;
    std::size_t receiveBuffer_ = 0;
};

}  // namespace protocol
