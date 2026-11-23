// Web data (io.md §6, §8): reading numbers out of JSON documents, parsing
// server-sent event streams, and the delay before a stream reconnects.
// No network and no JUCE, so the rules are tested on their own.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <espresso/primitives/io.h>

namespace server {

/// Largest JSON document or message read (io.md §8.3).
inline constexpr std::size_t maxDocumentBytes = std::size_t{1} << 20U;

/// How a document read.
enum class documentKind : std::uint8_t {
    read,      ///< Parsed; fieldsOf() reads its fields, some perhaps missing
    tooLarge,  ///< Larger than maxDocumentBytes: refused unread
    notJson,   ///< Not a JSON document
};

/// The numbers a document holds at a node's fields.
struct extraction {
    /// One value per field, or one for the whole document when there are
    /// no fields; a field that is absent or holds no number is marked
    /// missing and set to 0. The time is left at 0.
    primitives::dataFrame frame;
    /// Fields marked missing.
    std::size_t missing = 0;
};

/// A document parsed once, for every node that reads it.
struct document {
    /// How it read; `read` when json holds it.
    documentKind kind = documentKind::read;
    /// The document.
    nlohmann::json json;
};

/// @p d as a float, when it is finite and within a float's range (io.md
/// §8.2, §12.5).
std::optional<float> floatOf(double d) noexcept;

/// Parses @p text, refusing more than maxDocumentBytes (io.md §8.3).
document parseDocument(std::string_view text);

/// A field's JSON Pointer, parsed once; empty when the field is not a
/// valid pointer, which reads as missing.
using fieldPointer = std::optional<nlohmann::json::json_pointer>;

/// The pointers of @p fields; for no fields, the pointer to the whole
/// document. At most metadata::maxChannels.
std::vector<fieldPointer> fieldPointers(std::span<const std::string> fields);

/// Reads @p doc at @p pointers, the fields' JSON Pointers (io.md §8.1);
/// fieldPointers() of no fields reads the whole document. A number
/// converts to a float; `true` and `false` to 1 and 0; a string that reads
/// whole as a decimal number to that number. Anything else, a path that is
/// absent or not a valid pointer, and a number that is not finite as a
/// float are missing (§8.2).
extraction fieldsOf(const nlohmann::json& doc, std::span<const fieldPointer> pointers);

/// One message of a server-sent event stream.
struct sseMessage {
    /// Its event name; `message` when the stream gave none.
    std::string event;
    /// Its data lines, joined with line feeds.
    std::string data;
};

/// Parses a `text/event-stream` (the WHATWG HTML specification's
/// server-sent events): lines ending in CR, LF, or CRLF; fields `event`,
/// `data`, `id`, and `retry`; comments starting with `:`. A blank line
/// dispatches the message, when it has data. The parser holds at most
/// maxDocumentBytes of a message; a longer one is dropped and counted.
class sseParser {
public:
    /// Passes each message that @p bytes completes to @p message.
    void feed(std::string_view bytes, const std::function<void(const sseMessage&)>& message);
    /// The last event ID, sent back as `Last-Event-ID` on reconnection.
    const std::string& lastEventId() const noexcept { return lastEventId_; }
    /// The reconnection time the stream last gave; zero when it gave none.
    std::chrono::milliseconds retry() const noexcept { return retry_; }
    /// Messages dropped for their size since the last call.
    std::uint64_t takeTooLarge() noexcept { return std::exchange(tooLarge_, 0); }
    /// Starts a new connection: a partial line or message is discarded;
    /// the last event ID and retry time stay (§6.3).
    void reset();

private:
    void take(char c, const std::function<void(const sseMessage&)>& message);
    void line(std::string_view text, const std::function<void(const sseMessage&)>& message);

    std::string line_;
    bool afterCr_ = false;      // the last byte was a CR, so an LF after it ends nothing
    bool lineTooLong_ = false;  // the line being read passed maxDocumentBytes
    std::size_t bomAt_ = 0;     // bytes of a leading byte-order mark matched; 3 once past it
    bool dropping_ = false;     // the message being read is too large
    std::string event_;
    std::string data_;
    std::string lastEventId_;
    std::chrono::milliseconds retry_{0};
    std::uint64_t tooLarge_ = 0;
};

/// The delay before a stream reconnects when it was given no retry time
/// (io.md §6.3, §6.6).
inline constexpr std::chrono::milliseconds initialReconnect{3000};
/// The longest delay before a stream reconnects, unless its server asks
/// for longer.
inline constexpr std::chrono::milliseconds longestReconnect{60000};

/// The delay before a stream reconnects after @p failures failures in a
/// row, from 1: @p retry, or initialReconnect when it is zero, doubling
/// with each further failure up to longestReconnect, or to @p retry when
/// the server asked for longer.
std::chrono::milliseconds reconnectDelay(unsigned failures, std::chrono::milliseconds retry) noexcept;

}  // namespace server
