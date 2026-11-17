/// @file
/// OSC 1.0 message encoding and decoding (`protocol.md` §2).

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace protocol {

/// OSC blob argument (`b`).
struct blob {
    /// Blob contents.
    std::vector<std::uint8_t> bytes;
    /// Equal when the contents are equal.
    friend bool operator==(const blob&, const blob&) = default;
};

/// One OSC argument: `i`, `h`, `f`, `d`, `s`, or `b` (`protocol.md` §2.2).
using argument = std::variant<std::int32_t, std::int64_t, float, double, std::string, blob>;

/// Type tag character of @p arg.
char typeTag(const argument& arg) noexcept;

/// An OSC message: an address and its arguments.
struct message {
    /// Address, such as `/play`.
    std::string address;
    /// Arguments, in order.
    std::vector<argument> args;

    /// Equal when the address and arguments are equal.
    friend bool operator==(const message&, const message&) = default;

    /// Type tags of the arguments, without the leading comma.
    std::string tags() const;

    /// Typed access to argument @p i; nothing when absent or of another type.
    std::optional<std::int32_t> int32(std::size_t i) const;
    /// Argument @p i as a 64-bit integer; a 32-bit integer is widened.
    std::optional<std::int64_t> int64(std::size_t i) const;
    /// Argument @p i as a float; a double is narrowed.
    std::optional<float> float32(std::size_t i) const;
    /// Argument @p i as a string.
    std::optional<std::string_view> string(std::size_t i) const;
    /// The bytes of argument @p i, when it is a blob.
    std::optional<std::span<const std::uint8_t>> bytes(std::size_t i) const;
};

/// Encodes @p m as an OSC 1.0 packet.
std::vector<std::uint8_t> encode(const message& m);

/// Decodes one OSC 1.0 message. Returns nothing for malformed input,
/// bundles, and unsupported type tags; it never throws on bad data
/// (`protocol.md` §2.4).
std::optional<message> decode(std::span<const std::uint8_t> packet);

/// A short human-readable rendering, such as `/play 3 7 0.05 "none"`.
std::string describe(const message& m);

}  // namespace protocol
