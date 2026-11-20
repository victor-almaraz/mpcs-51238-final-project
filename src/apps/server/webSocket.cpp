#include "webSocket.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace server {

std::array<std::uint8_t, 20> sha1(std::span<const std::uint8_t> data) {
    std::array<std::uint32_t, 5> h{0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};
    // The message, a 1 bit, zeros, and its length in bits, in 64-byte blocks.
    std::vector<std::uint8_t> m(data.begin(), data.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8U;
    m.push_back(0x80U);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8)
        m.push_back(static_cast<std::uint8_t>(bits >> static_cast<unsigned>(shift)));
    std::array<std::uint32_t, 80> w{};
    for (std::size_t block = 0; block < m.size(); block += 64) {
        for (std::size_t t = 0; t < 16; ++t) {
            const std::size_t at = block + 4 * t;
            w[t] = (std::uint32_t{m[at]} << 24U) | (std::uint32_t{m[at + 1]} << 16U) |
                   (std::uint32_t{m[at + 2]} << 8U) | std::uint32_t{m[at + 3]};
        }
        for (std::size_t t = 16; t < 80; ++t)
            w[t] = std::rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
        auto [a, b, c, d, e] = h;
        for (std::size_t t = 0; t < 80; ++t) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (t < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999U;
            } else if (t < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (t < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            const std::uint32_t next = std::rotl(a, 5) + f + e + k + w[t];
            e = d;
            d = c;
            c = std::rotl(b, 30);
            b = a;
            a = next;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::array<std::uint8_t, 20> out{};
    for (std::size_t i = 0; i < 20; ++i) out[i] = static_cast<std::uint8_t>(h[i / 4] >> (24U - 8U * (i % 4)));
    return out;
}

std::string base64(std::span<const std::uint8_t> data) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::size_t n = std::min<std::size_t>(3, data.size() - i);
        std::uint32_t v = std::uint32_t{data[i]} << 16U;
        if (n > 1) v |= std::uint32_t{data[i + 1]} << 8U;
        if (n > 2) v |= std::uint32_t{data[i + 2]};
        for (std::size_t k = 0; k < 4; ++k) {
            out += k <= n ? alphabet[(v >> (18U - 6U * k)) & 0x3FU] : '=';
        }
    }
    return out;
}

std::string acceptKey(std::string_view clientKey) {
    // RFC 6455 §1.3: the key and the protocol's GUID, hashed and encoded.
    const std::string joined = std::string(clientKey) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::vector<std::uint8_t> bytes(joined.begin(), joined.end());
    const auto digest = sha1(bytes);
    return base64(digest);
}

std::string encodeFrame(opcode op, std::string_view payload, bool fin,
                        std::optional<std::array<std::uint8_t, 4>> mask) {
    std::string out;
    out += static_cast<char>((fin ? 0x80U : 0U) | static_cast<unsigned>(op));
    const std::uint8_t masked = mask ? 0x80U : 0U;
    const std::uint64_t n = payload.size();
    if (n < 126) {
        out += static_cast<char>(masked | n);
    } else if (n <= 0xFFFFU) {
        out += static_cast<char>(masked | 126U);
        out += static_cast<char>(n >> 8U);
        out += static_cast<char>(n & 0xFFU);
    } else {
        out += static_cast<char>(masked | 127U);
        for (int shift = 56; shift >= 0; shift -= 8)
            out += static_cast<char>((n >> static_cast<unsigned>(shift)) & 0xFFU);
    }
    if (!mask) return out + std::string(payload);
    for (const auto b : *mask) out += static_cast<char>(b);
    for (std::size_t i = 0; i < payload.size(); ++i)
        out += static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ (*mask)[i % 4]);
    return out;
}

frameError frameReader::feed(std::string_view bytes,
                             const std::function<void(const frameMessage&)>& message) {
    if (error_ != frameError::none) return error_;
    buffer_ += bytes;
    while (error_ == frameError::none) {
        const std::size_t before = buffer_.size();
        error_ = take(message);
        if (buffer_.size() == before) break;  // a frame is not complete yet
    }
    return error_;
}

// Reads one frame from the buffer when it holds a whole one.
frameError frameReader::take(const std::function<void(const frameMessage&)>& message) {
    const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(buffer_[i]); };
    if (buffer_.size() < 2) return frameError::none;
    const bool fin = (byte(0) & 0x80U) != 0;
    if ((byte(0) & 0x70U) != 0) return frameError::protocol;  // no extensions are negotiated
    const auto op = static_cast<opcode>(byte(0) & 0x0FU);
    const bool masked = (byte(1) & 0x80U) != 0;
    if (masked != masked_) return masked_ ? frameError::unmasked : frameError::protocol;
    std::uint64_t length = byte(1) & 0x7FU;
    std::size_t at = 2;
    if (length == 126) {
        if (buffer_.size() < 4) return frameError::none;
        length = (std::uint64_t{byte(2)} << 8U) | byte(3);
        at = 4;
    } else if (length == 127) {
        if (buffer_.size() < 10) return frameError::none;
        length = 0;
        for (std::size_t i = 2; i < 10; ++i) length = (length << 8U) | byte(i);
        at = 10;
    }
    const bool control = (static_cast<unsigned>(op) & 0x8U) != 0;
    if (control && (!fin || length > 125)) return frameError::protocol;
    if (!control && op != opcode::continuation && op != opcode::text && op != opcode::binary)
        return frameError::protocol;
    if (control && op != opcode::close && op != opcode::ping && op != opcode::pong)
        return frameError::protocol;
    // In size_t after the bound, so a length near 2^64 cannot wrap.
    if (!control && length > maxMessage_ - std::min(maxMessage_, message_.size()))
        return frameError::tooLarge;
    std::array<std::uint8_t, 4> mask{};
    if (masked) {
        if (buffer_.size() < at + 4) return frameError::none;
        for (std::size_t i = 0; i < 4; ++i) mask[i] = byte(at + i);
        at += 4;
    }
    const auto n = static_cast<std::size_t>(length);
    if (buffer_.size() < at + n) return frameError::none;
    std::string payload = buffer_.substr(at, n);
    if (masked) {
        for (std::size_t i = 0; i < n; ++i)
            payload[i] = static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ mask[i % 4]);
    }
    buffer_.erase(0, at + n);
    if (control) {
        message({.op = op, .payload = std::move(payload)});
        return frameError::none;
    }
    if (op == opcode::continuation) {
        if (!fragmented_) return frameError::protocol;
    } else {
        if (fragmented_) return frameError::protocol;  // a new message inside a fragmented one
        fragmented_ = op;
        message_.clear();
    }
    message_ += payload;
    if (fin) {
        message({.op = *fragmented_, .payload = std::move(message_)});
        message_.clear();
        fragmented_.reset();
    }
    return frameError::none;
}

}  // namespace server
