/// @file
/// Events carried by event pipes (`primitives.md` §5.4): MIDI messages,
/// triggers, values, and OSC messages, with MIDI byte conversion and
/// non-allocating OSC helpers.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include <proj/primitives/realtime.h>

namespace primitives {

/// What an event carries.
enum class eventType : std::uint16_t {
    noteOn = 1,       ///< MIDI note on: channel, note, velocity
    noteOff,          ///< MIDI note off: channel, note, velocity
    controlChange,    ///< MIDI control change: channel, controller, value
    pitchBend,        ///< MIDI pitch bend: channel, 14-bit value
    programChange,    ///< MIDI program change: channel, program
    channelPressure,  ///< MIDI channel pressure: channel, pressure
    polyPressure,     ///< MIDI polyphonic pressure: channel, note, pressure
    trigger,          ///< A trigger with a value, usually 1
    value,            ///< A number
    osc,              ///< An OSC message stored in the pipe's byte pool
};

/// One timestamped event (`primitives.md` §5.4.1).
struct event {
    /// Position in the block, from 0 to blockSize − 1.
    std::uint32_t sampleOffset = 0;
    /// What the payload holds.
    eventType type = eventType::trigger;
    /// Bytes used in the payload.
    std::uint16_t size = 0;
    /// Data; see the accessors below.
    std::array<std::byte, 24> payload{};
};

static_assert(std::is_trivially_copyable_v<event>);

/// Where an OSC event's message lives in its pipe's byte pool
/// (`primitives.md` §5.4.4).
struct poolRef {
    /// Offset of the first byte.
    std::uint32_t offset = 0;
    /// Length in bytes.
    std::uint32_t length = 0;
};

/// True for the MIDI event types.
constexpr bool isMidi(eventType t) noexcept { return t >= eventType::noteOn && t <= eventType::polyPressure; }

/// A MIDI event. @p channel is 0 to 15; data bytes are 0 to 127.
event midiEvent(std::uint32_t offset, eventType type, int channel, int data1,
                int data2 = 0) noexcept PROJ_NONBLOCKING;
/// A note-on event.
event noteOn(std::uint32_t offset, int channel, int note, int velocity) noexcept PROJ_NONBLOCKING;
/// A note-off event.
event noteOff(std::uint32_t offset, int channel, int note, int velocity = 0) noexcept PROJ_NONBLOCKING;
/// A pitch bend event; @p value is 0 to 16383, centered at 8192.
event pitchBend(std::uint32_t offset, int channel, int value) noexcept PROJ_NONBLOCKING;
/// A trigger event.
event trigger(std::uint32_t offset, float value = 1.0f) noexcept PROJ_NONBLOCKING;
/// A value event.
event valueEvent(std::uint32_t offset, float value) noexcept PROJ_NONBLOCKING;
/// An OSC event referring to pool data.
event oscEvent(std::uint32_t offset, poolRef data) noexcept PROJ_NONBLOCKING;

/// MIDI channel, 0 to 15.
int channel(const event& e) noexcept PROJ_NONBLOCKING;
/// First MIDI data byte: note, controller, or program.
int data1(const event& e) noexcept PROJ_NONBLOCKING;
/// Second MIDI data byte: velocity, value, or pressure.
int data2(const event& e) noexcept PROJ_NONBLOCKING;
/// Pitch bend value, 0 to 16383.
int bendValue(const event& e) noexcept PROJ_NONBLOCKING;
/// The number carried by a trigger or value event; for MIDI events, the
/// second data byte scaled to 0–1.
float number(const event& e) noexcept PROJ_NONBLOCKING;
/// The pool location of an OSC event's message.
poolRef oscData(const event& e) noexcept PROJ_NONBLOCKING;

/// Converts raw MIDI bytes (status and data) to an event. Running status
/// and system messages are not supported. A note-on with velocity 0 is a
/// note-off.
std::optional<event> fromMidi(std::span<const std::uint8_t> bytes,
                              std::uint32_t offset) noexcept PROJ_NONBLOCKING;

/// Writes the MIDI bytes of @p e into @p out. Returns the byte count, or
/// 0 when @p e is not a MIDI event. The channel is masked to 4 bits and
/// the data bytes to 7, so the bytes are always one valid message.
int toMidi(const event& e, std::array<std::uint8_t, 3>& out) noexcept PROJ_NONBLOCKING;

/// True when the OSC message's address is @p address. Real-time safe.
bool oscAddressIs(std::span<const std::byte> message, std::string_view address) noexcept PROJ_NONBLOCKING;

/// The first numeric argument (`i`, `f`, `h`, or `d`) of an OSC message.
std::optional<float> oscFirstNumber(std::span<const std::byte> message) noexcept PROJ_NONBLOCKING;

/// Encodes an OSC message with one float argument into @p out. Returns
/// the encoded size, or 0 when it does not fit.
std::size_t encodeOscFloat(std::span<std::byte> out, std::string_view address,
                           float value) noexcept PROJ_NONBLOCKING;

} // namespace primitives
