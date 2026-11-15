#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <arpa/inet.h>  // IWYU pragma: keep (htons, htonl, ntohs: POSIX declares them here)
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <proj/protocol/udp.h>

#include <poll.h>

namespace protocol {

namespace {

sockaddr_in loopback(std::uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return addr;
}

// The POSIX socket calls take a sockaddr_in as a pointer to the generic
// sockaddr: the narrow unsafe use at this boundary, confined to these two
// functions (modern-cpp skill, section 9). The pointer is passed straight
// to the call and never stored.
sockaddr* asSockaddr(sockaddr_in& addr) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the POSIX socket API takes sockaddr*
    return reinterpret_cast<sockaddr*>(&addr);
}

const sockaddr* asSockaddr(const sockaddr_in& addr) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the POSIX socket API takes sockaddr*
    return reinterpret_cast<const sockaddr*>(&addr);
}

} // namespace

// The POSIX socket calls also take options and lengths by pointer.
std::optional<udpSocket> udpSocket::bind(std::uint16_t port, std::string& error) {
    auto fail = [&error](std::string_view what) -> std::optional<udpSocket> {
        const int code = errno;  // before anything below can change it
        error = std::string(what) + ": " + std::system_category().message(code);
        return std::nullopt;
    };
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return fail("socket");
    udpSocket socket(fd, 0);  // owns the descriptor from here; closes it on every failure below
    // Larger buffers let a burst of datagrams through without loss. A
    // system limit can refuse a size (macOS: kern.ipc.maxsockbuf), so each
    // option halves the request until one is accepted. Linux accepts any
    // size and silently caps it (net.core.rmem_max); the effective receive
    // size is read back for receiveBufferBytes().
    for (const int option : {SO_RCVBUF, SO_SNDBUF}) {
        for (int size = static_cast<int>(requestedBufferBytes); size >= 64 * 1024; size /= 2) {
            if (::setsockopt(fd, SOL_SOCKET, option, &size, sizeof size) == 0) break;
        }
    }
    int effective = 0;
    socklen_t effectiveLen = sizeof effective;
    if (::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &effective, &effectiveLen) == 0 && effective > 0) {
        socket.receiveBuffer_ = static_cast<std::size_t>(effective);
    }
    sockaddr_in addr = loopback(port);
    if (::bind(fd, asSockaddr(addr), sizeof addr) != 0) return fail("bind 127.0.0.1");
    socklen_t len = sizeof addr;
    if (::getsockname(fd, asSockaddr(addr), &len) != 0) return fail("getsockname");
    socket.port_ = ntohs(addr.sin_port);
    return socket;
}

std::optional<udpSocket> udpSocket::bind(std::uint16_t port) {
    std::string ignored;
    return bind(port, ignored);
}

udpSocket::udpSocket(udpSocket&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), port_(other.port_), receiveBuffer_(other.receiveBuffer_) {}

udpSocket& udpSocket::operator=(udpSocket&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = std::exchange(other.fd_, -1);
        port_ = other.port_;
        receiveBuffer_ = other.receiveBuffer_;
    }
    return *this;
}

udpSocket::~udpSocket() {
    if (fd_ >= 0) ::close(fd_);
}

bool udpSocket::send(std::span<const std::uint8_t> bytes, endpoint to) const { return send(std::as_bytes(bytes), to); }

bool udpSocket::send(std::span<const std::byte> bytes, endpoint to) const {
    const sockaddr_in addr = loopback(to.port);
    const auto n = ::sendto(fd_, bytes.data(), bytes.size(), 0, asSockaddr(addr), sizeof addr);
    return std::cmp_equal(n, bytes.size());
}

std::optional<datagram> udpSocket::receive(int timeoutMs) const {
    pollfd p{.fd = fd_, .events = POLLIN, .revents = 0};
    const int ready = ::poll(&p, 1, timeoutMs);
    if (ready <= 0 || (static_cast<unsigned>(p.revents) & static_cast<unsigned>(POLLIN)) == 0u)
        return std::nullopt;
    datagram d;
    d.bytes.resize(65536);
    sockaddr_in from{};
    socklen_t len = sizeof from;
    const auto n = ::recvfrom(fd_, d.bytes.data(), d.bytes.size(), 0, asSockaddr(from), &len);
    if (n < 0) return std::nullopt;
    d.bytes.resize(static_cast<std::size_t>(n));
    d.from.port = ntohs(from.sin_port);
    return d;
}

} // namespace protocol
