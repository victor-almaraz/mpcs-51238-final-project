// Pages' messages and requests (io.md §12.4, §12.5, §12.11): OSC messages
// as JSON arrays in both directions, and HTTP/1.1 requests read from a byte
// stream. No sockets, so the rules are tested on their own.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace server {

/// An OSC message, encoded.
using oscBytes = std::vector<std::uint8_t>;

/// @p bytes, an encoded message held as std::byte (such as
/// primitives::oscPacket::used()), as oscBytes.
inline oscBytes bytesOf(std::span<const std::byte> bytes) {
    oscBytes out(bytes.size());
    std::ranges::transform(bytes, out.begin(), [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
    return out;
}

/// Largest request body a page may send (io.md §12.5); larger is 413.
inline constexpr std::size_t maxPageBody = std::size_t{64} << 10U;
/// Largest clip a page may upload, to `/clips/` (canvas.md §5.3).
inline constexpr std::size_t maxClipBody = std::size_t{1} << 20U;
/// Largest refused body read and dropped, so its sender reads the 413
/// rather than a reset; one larger is cut off.
inline constexpr std::size_t maxDrainedBody = std::size_t{16} << 20U;

/// The OSC messages a `POST /osc` body or a WebSocket text frame holds: a
/// JSON array of messages, each an array of an address and its arguments.
/// A number becomes an `f`, a string an `s`, and `true` and `false` an `i`
/// of 1 and 0 (io.md §12.5). Nothing, with the reason in @p error, when any
/// message breaks a rule, so a page never has part of a batch delivered.
std::optional<std::vector<oscBytes>> messagesFromPage(std::string_view json, std::string& error);

/// An encoded OSC message from a WebSocket binary frame (io.md §12.6), when
/// it is one: an address starting with `/`, readable arguments, and at
/// most 1024 bytes.
std::optional<oscBytes> binaryFromPage(std::span<const std::uint8_t> frame);

/// An encoded OSC message as a page reads it: a JSON array of its address
/// and arguments. `i`, `f`, `h`, and `d` become numbers (a non-finite one
/// null), `s` strings, `T` and `F` booleans, and other types null. Nothing
/// when the message cannot be read by protocol::oscRules::lenient.
std::optional<std::string> messageToPage(std::span<const std::uint8_t> osc);

/// An HTTP/1.1 request (io.md §12.11).
struct httpMessage {
    std::string method;
    /// The request target: a path, with its query.
    std::string target;
    /// Header names in lower case, with their values; a repeated header
    /// keeps the first.
    std::map<std::string, std::string, std::less<>> headers;
    std::string body;

    /// The value of header @p name (lower case), or empty; valid while the
    /// message is.
    std::string_view header(std::string_view name) const;
    /// The target's path, without its query.
    std::string_view path() const;
    /// The value of query parameter @p name, if it is given; valid while
    /// the message is.
    std::optional<std::string_view> query(std::string_view name) const;
};

/// Why a request cannot be read: the status to answer before closing.
enum class requestError : std::uint8_t {
    none,
    badRequest,       ///< 400: not an HTTP/1.1 request
    lengthRequired,   ///< 411: a POST or PUT, or a body, without Content-Length
    tooLarge,         ///< 413: a body over maxPageBody (maxClipBody for a clip)
    headersTooLarge,  ///< 431: a request head over 16 KiB
};

/// Reads requests from a connection's bytes, one after another (keep-alive).
class requestReader {
public:
    /// Adds @p bytes from the connection; those of a body refused as too
    /// large are dropped.
    void feed(std::string_view bytes);
    /// The next whole request, if one has arrived; an error stays.
    std::optional<httpMessage> next();
    /// Why the stream cannot be read, once it cannot.
    requestError error() const noexcept { return error_; }
    /// Bytes after the last request read: what an upgraded connection
    /// sent before its first frame.
    std::string takeRest() { return std::exchange(buffer_, {}); }
    /// Bytes of a body refused as too large still to arrive.
    std::size_t skipping() const noexcept { return skip_; }

private:
    std::string buffer_;
    std::size_t skip_ = 0;
    requestError error_ = requestError::none;
};

}  // namespace server
