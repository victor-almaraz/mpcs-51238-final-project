// SHA-1, base64, the WebSocket handshake key, and frames (RFC 3174,
// RFC 4648, RFC 6455; io.md §14.1).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "webSocket.h"

using namespace server;

namespace {

// The bytes of @p s, as SHA-1 and base64 take them.
std::vector<std::uint8_t> bytesOf(std::string_view s) { return {s.begin(), s.end()}; }

std::string hex(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    for (const auto b : bytes) {
        out += digits[b >> 4U];
        out += digits[b & 0xfU];
    }
    return out;
}

std::vector<frameMessage> read(frameReader& r, std::string_view bytes,
                               frameError expected = frameError::none) {
    std::vector<frameMessage> out;
    CHECK(r.feed(bytes, [&](const frameMessage& m) { out.push_back(m); }) == expected);
    return out;
}

constexpr std::array<std::uint8_t, 4> key{0x37, 0xfa, 0x21, 0x3d};
constexpr std::size_t mib = std::size_t{1} << 20U;

}  // namespace

TEST_CASE("SHA-1 matches RFC 3174's test vectors") {
    CHECK(hex(sha1(bytesOf("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex(sha1(bytesOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(hex(sha1(bytesOf(std::string(1'000'000, 'a')))) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    std::string repeated;
    for (int i = 0; i < 10; ++i)
        repeated += "0123456701234567012345670123456701234567012345670123456701234567";
    CHECK(hex(sha1(bytesOf(repeated))) == "dea356a2cddd90c7a7ecedc5ebb563934f460452");
    CHECK(hex(sha1(bytesOf(""))) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST_CASE("base64 matches RFC 4648's test vectors") {
    CHECK(base64(bytesOf("")).empty());
    CHECK(base64(bytesOf("f")) == "Zg==");
    CHECK(base64(bytesOf("fo")) == "Zm8=");
    CHECK(base64(bytesOf("foo")) == "Zm9v");
    CHECK(base64(bytesOf("foob")) == "Zm9vYg==");
    CHECK(base64(bytesOf("fooba")) == "Zm9vYmE=");
    CHECK(base64(bytesOf("foobar")) == "Zm9vYmFy");
}

TEST_CASE("the accept key matches RFC 6455's sample handshake") {
    CHECK(acceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("frames of every length form round trip masked and unmasked") {
    for (const std::size_t n :
         {std::size_t{0}, std::size_t{125}, std::size_t{126}, std::size_t{65535}, std::size_t{65536}}) {
        INFO(n);
        const std::string payload(n, 'p');
        frameReader client(true, mib);
        const auto fromClient = read(client, encodeFrame(opcode::text, payload, true, key));
        REQUIRE(fromClient.size() == 1);
        CHECK(fromClient[0].op == opcode::text);
        CHECK(fromClient[0].payload == payload);
        frameReader server(false, mib);
        const auto fromServer = read(server, encodeFrame(opcode::binary, payload));
        REQUIRE(fromServer.size() == 1);
        CHECK(fromServer[0].op == opcode::binary);
        CHECK(fromServer[0].payload.size() == n);
    }
    // RFC 6455 §5.7: a masked "Hello" from a client.
    CHECK(encodeFrame(opcode::text, "Hello", true, key) ==
          std::string("\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11));
    CHECK(encodeFrame(opcode::text, "Hello") == std::string("\x81\x05Hello", 7));
}

TEST_CASE("fragments join into one message and control frames pass between them") {
    frameReader r(true, mib);
    const std::string bytes = encodeFrame(opcode::text, "Hel", false, key) +
                              encodeFrame(opcode::ping, "x", true, key) +
                              encodeFrame(opcode::continuation, "lo", true, key);
    // Byte by byte, as a socket may deliver them.
    std::vector<frameMessage> got;
    for (const char c : bytes) {
        const auto more = read(r, std::string_view(&c, 1));
        got.insert(got.end(), more.begin(), more.end());
    }
    REQUIRE(got.size() == 2);
    CHECK(got[0].op == opcode::ping);
    CHECK(got[1].op == opcode::text);
    CHECK(got[1].payload == "Hello");
}

TEST_CASE("frame readers refuse unmasked client frames large messages and protocol errors") {
    frameReader unmasked(true, mib);
    read(unmasked, encodeFrame(opcode::text, "hi"), frameError::unmasked);
    frameReader large(true, mib / 16);
    read(large, encodeFrame(opcode::text, std::string(40000, 'a'), false, key));
    read(large, encodeFrame(opcode::continuation, std::string(40000, 'a'), true, key), frameError::tooLarge);
    frameReader reserved(true, mib);
    read(reserved, std::string("\xC1\x80", 2) + std::string(4, '\0'), frameError::protocol);
    frameReader longPing(true, mib);
    read(longPing, encodeFrame(opcode::ping, std::string(126, 'a'), true, key), frameError::protocol);
    frameReader fragmentedPing(true, mib);
    read(fragmentedPing, encodeFrame(opcode::ping, "a", false, key), frameError::protocol);
    frameReader orphan(true, mib);
    read(orphan, encodeFrame(opcode::continuation, "a", true, key), frameError::protocol);
    frameReader nested(true, mib);
    read(nested, encodeFrame(opcode::text, "a", false, key) + encodeFrame(opcode::text, "b", true, key),
         frameError::protocol);
    // An error stays.
    read(nested, encodeFrame(opcode::text, "c", true, key), frameError::protocol);
}
