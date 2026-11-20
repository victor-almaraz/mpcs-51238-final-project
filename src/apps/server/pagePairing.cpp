#include "pagePairing.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "httpText.h"

#include <ifaddrs.h>
#include <net/if.h>

namespace server {

std::string pairingToken() {
    std::array<unsigned char, 16> bytes{};
    std::ifstream random("/dev/urandom", std::ios::binary);
    // istream::read takes bytes as char (modern-cpp skill §9).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    random.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!random) throw std::runtime_error("cannot read /dev/urandom for the pairing token");
    constexpr std::string_view hex = "0123456789abcdef";
    std::string out;
    for (const unsigned char b : bytes) {
        out += hex[b >> 4U];
        out += hex[b & 0xFU];
    }
    return out;
}

bool sameToken(std::string_view a, std::string_view b) noexcept {
    // Every byte of the longer is compared, against zero past the shorter.
    unsigned difference = a.size() == b.size() ? 0U : 1U;
    const std::size_t n = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0U;
        const auto y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0U;
        difference |= static_cast<unsigned>(x ^ y);
    }
    return difference == 0;
}

std::optional<std::string> cookieValue(std::string_view header, std::string_view name) {
    const auto value = fieldIn(header, ';', name);
    if (!value) return std::nullopt;
    return std::string(*value);
}

std::vector<std::string> lanAddresses() {
    ifaddrs* raw = nullptr;
    // getifaddrs returns a list it allocated, freed with freeifaddrs
    // (modern-cpp skill §9).
    if (::getifaddrs(&raw) != 0) return {};
    const std::unique_ptr<ifaddrs, decltype(&::freeifaddrs)> list(raw, &::freeifaddrs);
    std::vector<std::string> out;
    for (const ifaddrs* i = list.get(); i != nullptr; i = i->ifa_next) {
        if (i->ifa_addr == nullptr || i->ifa_addr->sa_family != AF_INET) continue;
        if ((i->ifa_flags & static_cast<unsigned>(IFF_UP)) == 0 ||
            (i->ifa_flags & static_cast<unsigned>(IFF_LOOPBACK)) != 0)
            continue;
        std::array<char, INET_ADDRSTRLEN> text{};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): an AF_INET address is a sockaddr_in
        const auto* in = reinterpret_cast<const sockaddr_in*>(i->ifa_addr);
        if (::inet_ntop(AF_INET, &in->sin_addr, text.data(), text.size()) == nullptr) continue;
        std::string address(text.data());
        if (std::ranges::find(out, address) == out.end()) out.push_back(std::move(address));
    }
    return out;
}

std::vector<std::string> lanNames() {
    std::vector<std::string> out = lanAddresses();
    std::array<char, 256> name{};
    if (::gethostname(name.data(), name.size()) == 0) {
        std::string host(name.data());
        if (const auto dot = host.find('.'); dot != std::string::npos) host.resize(dot);
        if (!host.empty()) out.push_back(host + ".local");
    }
    return out;
}

bool hostAllowed(std::string_view host, std::uint16_t port, const std::vector<std::string>& names) {
    const std::string suffix = ":" + std::to_string(port);
    if (!host.ends_with(suffix)) return false;
    const std::string_view name = host.substr(0, host.size() - suffix.size());
    if (name == "127.0.0.1" || name == "localhost") return true;
    // Host names compare without case; .local names are often typed so.
    const std::string typed = lowered(name);
    return std::ranges::any_of(names, [&](const std::string& n) { return lowered(n) == typed; });
}

bool isLoopback(std::string_view address) noexcept {
    if (address == "::1") return true;
    if (address.starts_with("::ffff:")) address.remove_prefix(7);
    return address.starts_with("127.");
}

pairing pairingFor(std::string_view peer, std::string_view cookieHeader, std::optional<std::string_view> pair,
                   std::string_view token) {
    if (isLoopback(peer)) return pairing::allowed;
    if (pair && sameToken(*pair, token)) return pairing::pair;
    const auto cookie = cookieValue(cookieHeader, pairCookie);
    return cookie && sameToken(*cookie, token) ? pairing::allowed : pairing::refused;
}

}  // namespace server
