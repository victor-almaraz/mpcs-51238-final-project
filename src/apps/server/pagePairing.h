// Pages on the network (io.md §12.7, §12.8): which Hosts and Origins the
// page listener answers, which peers need the pairing token, and the token
// itself. Functions of addresses and headers, so they are tested without a
// second machine.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace server {

/// A 128-bit token from the operating system's random source, as 32 hex
/// digits; a new one each time the server starts.
std::string pairingToken();

/// True when @p a and @p b are equal, comparing every byte whatever the
/// first difference, so the time taken tells nothing of the token.
bool sameToken(std::string_view a, std::string_view b) noexcept;

/// The value of cookie @p name in a `Cookie` header, if it has one.
std::optional<std::string> cookieValue(std::string_view header, std::string_view name);

/// The machine's IPv4 addresses other than loopback, from its interfaces
/// that are up, in interface order.
std::vector<std::string> lanAddresses();

/// The names a page on the network reaches the machine by: its addresses,
/// and `<hostname>.local`.
std::vector<std::string> lanNames();

/// True when @p host, a request's Host header, names the listener on
/// @p port: `127.0.0.1:P` or `localhost:P`, or on a LAN listener, one of
/// @p names at P (io.md §12.7).
bool hostAllowed(std::string_view host, std::uint16_t port, const std::vector<std::string>& names);

/// True when @p address, a peer's IPv4 or IPv6 address as text, is a
/// loopback address: 127.0.0.0/8, ::1, or ::ffff:127.x.y.z.
bool isLoopback(std::string_view address) noexcept;

/// What the pairing rule says of a request on a LAN listener (io.md §12.8).
enum class pairing : std::uint8_t {
    allowed,  ///< A loopback peer, or one with the cookie
    pair,     ///< The pairing link: set the cookie and redirect without the query
    refused,  ///< 401: a peer other than loopback without the cookie
};

/// The pairing rule for a request from @p peer carrying @p cookieHeader and
/// the query parameter @p pair, on a listener whose token is @p token.
pairing pairingFor(std::string_view peer, std::string_view cookieHeader, std::optional<std::string_view> pair,
                   std::string_view token);

/// The name of the pairing cookie.
inline constexpr std::string_view pairCookie = "espresso-pair";

}  // namespace server
