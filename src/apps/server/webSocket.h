// The server side of WebSocket (RFC 6455): the opening handshake's key,
// with SHA-1 (RFC 3174) and base64, and reading and writing frames. No
// sockets and no library, so the page bridge (io.md §12.6, §12.11) and the
// tests' local server share it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace server {

/// The SHA-1 digest of @p data (RFC 3174).
std::array<std::uint8_t, 20> sha1(std::span<const std::uint8_t> data);

/// @p data in base64 (RFC 4648), with padding.
std::string base64(std::span<const std::uint8_t> data);

/// The `Sec-WebSocket-Accept` value for a client's `Sec-WebSocket-Key`
/// (RFC 6455 §4.2.2).
std::string acceptKey(std::string_view clientKey);

/// Frame opcodes (RFC 6455 §5.2).
enum class opcode : std::uint8_t {
    continuation = 0x0,
    text = 0x1,
    binary = 0x2,
    close = 0x8,
    ping = 0x9,
    pong = 0xA,
};

/// A frame: unmasked, with a payload of any length, at most 2^63 bytes.
std::string encodeFrame(opcode op, std::string_view payload, bool fin = true,
                        std::optional<std::array<std::uint8_t, 4>> mask = std::nullopt);

/// What reading frames found.
enum class frameError : std::uint8_t {
    none,
    unmasked,  ///< A client frame without a mask (close with 1002)
    tooLarge,  ///< A message over the reader's limit (close with 1009)
    protocol,  ///< Reserved bits, an unknown opcode, a fragmented or long control frame (close with 1002)
};

/// A whole message, joined from its fragments, or a control frame.
struct frameMessage {
    /// text, binary, close, ping, or pong.
    opcode op = opcode::text;
    /// The payload, unmasked.
    std::string payload;
};

/// Reads frames from a byte stream: unmasks them, joins fragments into
/// messages up to a limit, and passes control frames as they come.
class frameReader {
public:
    /// Reads frames that must be masked when @p masked (from a client),
    /// with messages of at most @p maxMessage bytes.
    frameReader(bool masked, std::size_t maxMessage) noexcept : masked_(masked), maxMessage_(maxMessage) {}

    /// Passes each message that @p bytes completes to @p message; stops
    /// at the first error, which stays.
    frameError feed(std::string_view bytes, const std::function<void(const frameMessage&)>& message);

private:
    frameError take(const std::function<void(const frameMessage&)>& message);

    bool masked_;
    std::size_t maxMessage_;
    std::string buffer_;
    std::size_t read_ = 0;              // where the first frame not yet read starts in buffer_
    std::optional<opcode> fragmented_;  // the opcode of a message whose fragments are arriving
    std::string message_;
    frameError error_ = frameError::none;
};

}  // namespace server
