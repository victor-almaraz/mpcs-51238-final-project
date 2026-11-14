#include <proj/protocol/udp.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace protocol {

namespace {

sockaddr_in loopback(std::uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return addr;
}

} // namespace

// The POSIX socket calls take addresses and options by pointer and
// sockaddr_in through a reinterpret_cast: a narrow unsafe use, confined to
// this file (see the modern-cpp skill).
std::optional<udpSocket> udpSocket::bind(std::uint16_t port, std::string& error) {
    auto fail = [&error](std::string_view what) -> std::optional<udpSocket> {
        error = std::string(what) + ": " + std::strerror(errno);
        return std::nullopt;
    };
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return fail("socket");
    udpSocket socket(fd, 0);  // owns the descriptor from here; closes it on every failure below
    // Larger buffers let a burst of IR chunks through without loss.
    const int size = 4 * 1024 * 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof size);
    sockaddr_in addr = loopback(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) return fail("bind 127.0.0.1");
    socklen_t len = sizeof addr;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    socket.port_ = ntohs(addr.sin_port);
    return socket;
}

std::optional<udpSocket> udpSocket::bind(std::uint16_t port) {
    std::string ignored;
    return bind(port, ignored);
}

udpSocket::udpSocket(udpSocket&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), port_(other.port_) {}

udpSocket& udpSocket::operator=(udpSocket&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = std::exchange(other.fd_, -1);
        port_ = other.port_;
    }
    return *this;
}

udpSocket::~udpSocket() {
    if (fd_ >= 0) ::close(fd_);
}

bool udpSocket::send(std::span<const std::uint8_t> bytes, endpoint to) const { return send(std::as_bytes(bytes), to); }

bool udpSocket::send(std::span<const std::byte> bytes, endpoint to) const {
    const sockaddr_in addr = loopback(to.port);
    const ssize_t n = ::sendto(fd_, bytes.data(), bytes.size(), 0,
                               reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
    return n == static_cast<ssize_t>(bytes.size());
}

std::optional<datagram> udpSocket::receive(int timeoutMs) const {
    pollfd p{fd_, POLLIN, 0};
    const int ready = ::poll(&p, 1, timeoutMs);
    if (ready <= 0 || !(p.revents & POLLIN)) return std::nullopt;
    datagram d;
    d.bytes.resize(65536);
    sockaddr_in from{};
    socklen_t len = sizeof from;
    const ssize_t n = ::recvfrom(fd_, d.bytes.data(), d.bytes.size(), 0,
                                 reinterpret_cast<sockaddr*>(&from), &len);
    if (n < 0) return std::nullopt;
    d.bytes.resize(static_cast<std::size_t>(n));
    d.from.port = ntohs(from.sin_port);
    return d;
}

} // namespace protocol
