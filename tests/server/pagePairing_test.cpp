// The page listener on the network: Hosts, peers, the pairing token, and
// its cookie (io.md §12.7, §12.8, §14.1). Functions of addresses, so most
// run without a second machine; the last reaches the listener through
// this machine's own LAN address, when it has one.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "pageBridge.h"
#include "pagePairing.h"
#include "support/sockets.h"
#include "support/tempDir.h"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using namespace server;

TEST_CASE("pairing tokens are 128 bits of hex and new each time (io.md 12.8)") {
    std::set<std::string> seen;
    for (int i = 0; i < 20; ++i) {
        const auto t = pairingToken();
        CHECK(t.size() == 32);
        CHECK(t.find_first_not_of("0123456789abcdef") == std::string::npos);
        seen.insert(t);
    }
    CHECK(seen.size() == 20);
}

TEST_CASE("tokens compare whole whatever the first difference") {
    CHECK(sameToken("abc", "abc"));
    CHECK_FALSE(sameToken("abc", "abd"));
    CHECK_FALSE(sameToken("abc", "abcd"));
    CHECK_FALSE(sameToken("", "a"));
    CHECK(sameToken("", ""));
}

TEST_CASE("cookies are read by name from a Cookie header") {
    CHECK(cookieValue("espresso-pair=abc", "espresso-pair") == "abc");
    CHECK(cookieValue("theme=dark; espresso-pair=abc; x=1", "espresso-pair") == "abc");
    CHECK_FALSE(cookieValue("espresso-pairs=abc", "espresso-pair"));
    CHECK_FALSE(cookieValue("", "espresso-pair"));
}

TEST_CASE("a page listener answers its own names at its port and nothing else (io.md 12.7)") {
    CHECK(hostAllowed("127.0.0.1:8000", 8000, {}));
    CHECK(hostAllowed("localhost:8000", 8000, {}));
    CHECK_FALSE(hostAllowed("localhost:8001", 8000, {}));
    CHECK_FALSE(hostAllowed("localhost", 8000, {}));
    CHECK_FALSE(hostAllowed("192.168.1.20:8000", 8000, {}));  // a loopback listener knows no LAN names
    const std::vector<std::string> lan{"192.168.1.20", "studio.local"};
    CHECK(hostAllowed("192.168.1.20:8000", 8000, lan));
    CHECK(hostAllowed("Studio.local:8000", 8000, lan));
    CHECK_FALSE(hostAllowed("192.168.1.21:8000", 8000, lan));
    CHECK_FALSE(hostAllowed("evil.example:8000", 8000, lan));  // DNS rebinding
}

TEST_CASE("only loopback peers go without the token") {
    for (const std::string_view a : {"127.0.0.1", "127.8.9.10", "::1", "::ffff:127.0.0.1"})
        CHECK(isLoopback(a));
    for (const std::string_view a : {"192.168.1.20", "10.0.0.1", "::ffff:10.0.0.1", "", "fe80::1"})
        CHECK_FALSE(isLoopback(a));
    const std::string token = "0123456789abcdef0123456789abcdef";
    CHECK(pairingFor("127.0.0.1", "", std::nullopt, token) == pairing::allowed);
    CHECK(pairingFor("192.168.1.30", "", std::nullopt, token) == pairing::refused);
    CHECK(pairingFor("192.168.1.30", "", std::string("wrong"), token) == pairing::refused);
    CHECK(pairingFor("192.168.1.30", "", token, token) == pairing::pair);
    CHECK(pairingFor("192.168.1.30", "espresso-pair=" + token, std::nullopt, token) == pairing::allowed);
    CHECK(pairingFor("192.168.1.30", "espresso-pair=nope", std::nullopt, token) == pairing::refused);
}

namespace {

// One request to @p address:@p port, and its response's head.
std::string ask(const std::string& address, std::uint16_t port, const std::string& request) {
    support::httpClient c(address, port);
    REQUIRE(c.connected());
    c.write(request);
    const auto got =
        c.readUntil([](const std::string& b) { return b.find("\r\n\r\n") != std::string::npos; });
    return got.substr(0, got.find("\r\n\r\n") + 4);
}

}  // namespace

TEST_CASE("a LAN listener pairs a peer other than loopback by its link and cookie (io.md 12.8)") {
    const auto addresses = lanAddresses();
    if (addresses.empty()) SKIP("this machine has no address other than loopback");
    const support::tempDir dir;
    std::filesystem::create_directories(dir / "ui");
    std::ofstream(dir / "ui" / "index.html") << "<h1>keys</h1>";
    std::vector<std::string> announced;
    pageBridge bridge({.root = dir.path(),
                       .helper = {},
                       .report = {},
                       .pingInterval = std::chrono::milliseconds(10000),
                       .lan = true,
                       .token = "0123456789abcdef0123456789abcdef",
                       .announce = [&](const std::vector<std::string>& links) { announced = links; }});
    bridge.listen(0);
    const auto port = bridge.port();
    REQUIRE(port != 0);
    const std::string& lan = addresses.front();
    const std::string host = lan + ":" + std::to_string(port);
    REQUIRE_FALSE(announced.empty());
    CHECK(announced.front() == "http://" + host + "/?pair=0123456789abcdef0123456789abcdef");

    // Loopback needs no token.
    CHECK_THAT(
        ask("127.0.0.1", port, "GET / HTTP/1.1\r\nHost: localhost:" + std::to_string(port) + "\r\n\r\n"),
        StartsWith("HTTP/1.1 200 "));
    // From the LAN address: no cookie, 401; the link, a cookie and a redirect; the cookie, the page.
    CHECK_THAT(ask(lan, port, "GET / HTTP/1.1\r\nHost: " + host + "\r\n\r\n"), StartsWith("HTTP/1.1 401 "));
    const auto paired =
        ask(lan, port, "GET /?pair=0123456789abcdef0123456789abcdef HTTP/1.1\r\nHost: " + host + "\r\n\r\n");
    CHECK_THAT(paired, StartsWith("HTTP/1.1 302 "));
    CHECK_THAT(paired, ContainsSubstring("Location: /\r\n"));
    CHECK_THAT(paired, ContainsSubstring("Set-Cookie: espresso-pair=0123456789abcdef0123456789abcdef; "
                                         "HttpOnly; SameSite=Strict; Path=/\r\n"));
    CHECK_THAT(ask(lan, port,
                   "GET / HTTP/1.1\r\nHost: " + host +
                       "\r\nCookie: espresso-pair=0123456789abcdef0123456789abcdef\r\n\r\n"),
               StartsWith("HTTP/1.1 200 "));
    CHECK_THAT(ask(lan, port, "GET /?pair=guess HTTP/1.1\r\nHost: " + host + "\r\n\r\n"),
               StartsWith("HTTP/1.1 401 "));
    // A Host that is not the machine's is refused before any token.
    CHECK_THAT(ask(lan, port, "GET / HTTP/1.1\r\nHost: evil.example:" + std::to_string(port) + "\r\n\r\n"),
               StartsWith("HTTP/1.1 403 "));
}
