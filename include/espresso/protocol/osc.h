/// @file
/// OSC message encoding and decoding (`protocol.md` §2).

#pragma once

#include <array>
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

/// OSC 1.1 nil argument (`N`): no value.
struct nil {
    /// Every nil is equal.
    friend bool operator==(const nil&, const nil&) = default;
};

/// OSC 1.1 impulse argument (`I`, "infinitum" in OSC 1.0): an event with
/// no value.
struct impulse {
    /// Every impulse is equal.
    friend bool operator==(const impulse&, const impulse&) = default;
};

/// OSC character argument (`c`): an ASCII character, sent as 32 bits.
struct character {
    /// The 32 bits sent.
    std::uint32_t code = 0;
    /// Equal when the codes are equal.
    friend bool operator==(const character&, const character&) = default;
};

/// OSC colour argument (`r`): 32-bit RGBA.
struct rgba {
    /// Red in the most significant byte, then green, blue, and alpha.
    std::uint32_t value = 0;
    /// Equal when the values are equal.
    friend bool operator==(const rgba&, const rgba&) = default;
};

/// OSC MIDI message argument (`m`).
struct midiMessage {
    /// Port ID, status byte, and two data bytes, in the order sent.
    std::array<std::uint8_t, 4> bytes{};
    /// Equal when the bytes are equal.
    friend bool operator==(const midiMessage&, const midiMessage&) = default;
};

/// OSC time tag argument (`t`), in NTP format.
struct timeTag {
    /// Seconds since 1900 in the high 32 bits, fractions of a second in
    /// the low 32.
    std::uint64_t value = 0;
    /// Equal when the values are equal.
    friend bool operator==(const timeTag&, const timeTag&) = default;
};

/// OSC symbol argument (`S`): a string that names something.
struct symbol {
    /// The symbol's text.
    std::string name;
    /// Equal when the names are equal.
    friend bool operator==(const symbol&, const symbol&) = default;
};

/// One OSC argument: `i`, `h`, `f`, `d`, `s`, or `b`, the client and
/// server's own types (`protocol.md` §2.2), or one of the OSC 1.1 types
/// that other programs send: `T` and `F` (a `bool`), `N`, `I`, `c`, `r`,
/// `m`, `t`, or `S` (§2.5).
using argument = std::variant<std::int32_t, std::int64_t, float, double, std::string, blob, bool, nil,
                              impulse, character, rgba, midiMessage, timeTag, symbol>;

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
    /// Argument @p i as a double; a float is widened.
    std::optional<double> float64(std::size_t i) const;
    /// Argument @p i as a string.
    std::optional<std::string_view> string(std::size_t i) const;
    /// The bytes of argument @p i, when it is a blob.
    std::optional<std::span<const std::uint8_t>> bytes(std::size_t i) const;
};

/// Encodes @p m as an OSC packet.
std::vector<std::uint8_t> encode(const message& m);

/// The rules a packet is decoded by.
enum class oscRules : std::uint8_t {
    /// The client and server's own messages (`protocol.md` §2.1, §2.2):
    /// the type tags `i h f d s b`, padding of zeros, and nothing after
    /// the last argument.
    control,
    /// Messages from other programs, such as those a piece sends to a page
    /// (`protocol.md` §2.5, `io.md` §12.5): every type tag of OSC 1.1; a
    /// message without a type tag string has no arguments, padding is
    /// skipped unread, and bytes after the last argument are ignored.
    lenient,
};

/// Decodes one OSC message by @p rules. Returns nothing for malformed
/// input, bundles, and type tags the rules do not accept; it never throws
/// on bad data (`protocol.md` §2.4).
std::optional<message> decode(std::span<const std::uint8_t> packet, oscRules rules = oscRules::control);

/// A short human-readable rendering, such as `/play 3 7 0.05 "none"`.
std::string describe(const message& m);

}  // namespace protocol
