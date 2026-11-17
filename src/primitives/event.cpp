// Event construction, MIDI conversion, and OSC helpers (primitives.md §5.4).

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <proj/primitives/event.h>
#include <proj/primitives/realtime.h>

namespace primitives {

namespace {

template <typename T>
void put(event& e, std::size_t at, T value) noexcept {
    const auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    std::ranges::copy(bytes, std::span(e.payload).subspan(at, sizeof(T)).begin());
}

template <typename T>
T get(const event& e, std::size_t at) noexcept {
    std::array<std::byte, sizeof(T)> bytes{};
    std::ranges::copy(std::span(e.payload).subspan(at, sizeof(T)), bytes.begin());
    return std::bit_cast<T>(bytes);
}

// Payload byte @p k, unsigned for bit manipulation.
std::uint32_t byteAt(const event& e, std::size_t k) noexcept {
    return std::to_integer<std::uint32_t>(e.payload[k]);
}

int clamp7(int v) noexcept { return std::clamp(v, 0, 127); }

// OSC strings are null-terminated and padded to 4 bytes.
std::size_t padded(std::size_t n) noexcept { return (n + 4) & ~std::size_t{3}; }

// Big-endian integers starting at @p at; the caller checks the length.
std::uint32_t readBig32(std::span<const std::byte> m, std::size_t at) noexcept {
    std::uint32_t v = 0;
    for (std::size_t k = 0; k < 4; ++k) v = (v << 8U) | std::to_integer<std::uint32_t>(m[at + k]);
    return v;
}

std::uint64_t readBig64(std::span<const std::byte> m, std::size_t at) noexcept {
    return (std::uint64_t{readBig32(m, at)} << 32U) | readBig32(m, at + 4);
}

// Length of the string starting at @p at, or nothing when unterminated.
std::optional<std::size_t> stringLength(std::span<const std::byte> m, std::size_t at) noexcept {
    if (at >= m.size()) return std::nullopt;
    const auto rest = m.subspan(at);
    const auto end = std::ranges::find(rest, std::byte{0});
    if (end == rest.end()) return std::nullopt;
    return static_cast<std::size_t>(end - rest.begin());
}

// Length of the message's address, or nothing when it has none.
std::optional<std::size_t> addressLength(std::span<const std::byte> m) noexcept {
    const auto n = stringLength(m, 0);
    if (!n || *n == 0 || m[0] != std::byte{'/'}) return std::nullopt;
    return n;
}

} // namespace

event midiEvent(std::uint32_t offset, eventType type, int channel, int data1,
                int data2) noexcept PROJ_NONBLOCKING {
    event e;
    e.sampleOffset = offset;
    e.type = type;
    e.size = 3;
    e.payload[0] = static_cast<std::byte>(std::clamp(channel, 0, 15));
    e.payload[1] = static_cast<std::byte>(clamp7(data1));
    e.payload[2] = static_cast<std::byte>(clamp7(data2));
    return e;
}

event noteOn(std::uint32_t offset, int ch, int note, int velocity) noexcept PROJ_NONBLOCKING {
    return midiEvent(offset, eventType::noteOn, ch, note, velocity);
}

event noteOff(std::uint32_t offset, int ch, int note, int velocity) noexcept PROJ_NONBLOCKING {
    return midiEvent(offset, eventType::noteOff, ch, note, velocity);
}

event pitchBend(std::uint32_t offset, int ch, int value) noexcept PROJ_NONBLOCKING {
    const auto v = static_cast<std::uint32_t>(std::clamp(value, 0, 16383));
    return midiEvent(offset, eventType::pitchBend, ch, static_cast<int>(v & 0x7fU),
                     static_cast<int>(v >> 7U));
}

event trigger(std::uint32_t offset, float value) noexcept PROJ_NONBLOCKING {
    event e;
    e.sampleOffset = offset;
    e.type = eventType::trigger;
    e.size = sizeof value;
    put(e, 0, value);
    return e;
}

event valueEvent(std::uint32_t offset, float value) noexcept PROJ_NONBLOCKING {
    event e = trigger(offset, value);
    e.type = eventType::value;
    return e;
}

event oscEvent(std::uint32_t offset, poolRef data) noexcept PROJ_NONBLOCKING {
    event e;
    e.sampleOffset = offset;
    e.type = eventType::osc;
    e.size = sizeof data;
    put(e, 0, data);
    return e;
}

int channel(const event& e) noexcept PROJ_NONBLOCKING { return std::to_integer<int>(e.payload[0]); }
int data1(const event& e) noexcept PROJ_NONBLOCKING { return std::to_integer<int>(e.payload[1]); }
int data2(const event& e) noexcept PROJ_NONBLOCKING { return std::to_integer<int>(e.payload[2]); }
int bendValue(const event& e) noexcept PROJ_NONBLOCKING {
    return static_cast<int>(byteAt(e, 1) | (byteAt(e, 2) << 7U));
}

float number(const event& e) noexcept PROJ_NONBLOCKING {
    if (e.type == eventType::trigger || e.type == eventType::value) return get<float>(e, 0);
    if (e.type == eventType::pitchBend) return static_cast<float>(bendValue(e)) / 16383.0f;
    if (e.type == eventType::programChange || e.type == eventType::channelPressure) {
        return static_cast<float>(data1(e)) / 127.0f;
    }
    if (isMidi(e.type)) return static_cast<float>(data2(e)) / 127.0f;
    return 0.0f;
}

poolRef oscData(const event& e) noexcept PROJ_NONBLOCKING { return get<poolRef>(e, 0); }

std::optional<event> fromMidi(std::span<const std::uint8_t> bytes,
                              std::uint32_t offset) noexcept PROJ_NONBLOCKING {
    if (bytes.empty() || bytes[0] < 0x80 || bytes[0] >= 0xf0) return std::nullopt;
    const std::uint32_t lead = bytes[0];
    const std::uint32_t status = lead & 0xf0U;
    const auto ch = static_cast<int>(lead & 0x0fU);
    const std::size_t need = (status == 0xc0 || status == 0xd0) ? 2 : 3;
    if (bytes.size() < need) return std::nullopt;
    const auto d1 = static_cast<int>(std::uint32_t{bytes[1]} & 0x7fU);
    const auto d2 = need == 3 ? static_cast<int>(std::uint32_t{bytes[2]} & 0x7fU) : 0;
    switch (status) {
    case 0x80: return noteOff(offset, ch, d1, d2);
    case 0x90: return d2 == 0 ? noteOff(offset, ch, d1) : noteOn(offset, ch, d1, d2);
    case 0xa0: return midiEvent(offset, eventType::polyPressure, ch, d1, d2);
    case 0xb0: return midiEvent(offset, eventType::controlChange, ch, d1, d2);
    case 0xc0: return midiEvent(offset, eventType::programChange, ch, d1);
    case 0xd0: return midiEvent(offset, eventType::channelPressure, ch, d1);
    case 0xe0: return midiEvent(offset, eventType::pitchBend, ch, d1, d2);
    default: return std::nullopt;
    }
}

int toMidi(const event& e, std::array<std::uint8_t, 3>& out) noexcept PROJ_NONBLOCKING {
    std::uint32_t status = 0;
    int size = 3;
    switch (e.type) {
    case eventType::noteOff: status = 0x80; break;
    case eventType::noteOn: status = 0x90; break;
    case eventType::polyPressure: status = 0xa0; break;
    case eventType::controlChange: status = 0xb0; break;
    case eventType::programChange: status = 0xc0; size = 2; break;
    case eventType::channelPressure: status = 0xd0; size = 2; break;
    case eventType::pitchBend: status = 0xe0; break;
    default: return 0;
    }
    // Masked, so a malformed payload cannot change the message type.
    out = {static_cast<std::uint8_t>(status | (byteAt(e, 0) & 0x0fU)),
           static_cast<std::uint8_t>(byteAt(e, 1) & 0x7fU),
           static_cast<std::uint8_t>(size == 3 ? byteAt(e, 2) & 0x7fU : 0U)};
    return size;
}

bool oscAddressIs(std::span<const std::byte> m, std::string_view address) noexcept PROJ_NONBLOCKING {
    const auto n = addressLength(m);
    return n && *n == address.size() &&
           std::equal(address.begin(), address.end(), m.begin(), [](char c, std::byte b) { return std::byte(c) == b; });
}

std::optional<float> oscFirstNumber(std::span<const std::byte> m) noexcept PROJ_NONBLOCKING {
    const auto addr = addressLength(m);
    if (!addr) return std::nullopt;
    const std::size_t tagsAt = padded(*addr);
    const auto tagCount = stringLength(m, tagsAt);
    if (!tagCount || *tagCount == 0 || m[tagsAt] != std::byte{','}) return std::nullopt;
    std::size_t at = tagsAt + padded(*tagCount);
    for (std::size_t k = 1; k < *tagCount; ++k) {
        switch (static_cast<char>(m[tagsAt + k])) {
        case 'i':
            if (at + 4 > m.size()) return std::nullopt;
            return static_cast<float>(static_cast<std::int32_t>(readBig32(m, at)));
        case 'f':
            if (at + 4 > m.size()) return std::nullopt;
            return std::bit_cast<float>(readBig32(m, at));
        case 'h':
            if (at + 8 > m.size()) return std::nullopt;
            return static_cast<float>(static_cast<std::int64_t>(readBig64(m, at)));
        case 'd':
            if (at + 8 > m.size()) return std::nullopt;
            return static_cast<float>(std::bit_cast<double>(readBig64(m, at)));
        case 's': {
            const auto n = stringLength(m, at);
            if (!n) return std::nullopt;
            at += padded(*n);
            break;
        }
        case 'b': {
            if (at + 4 > m.size()) return std::nullopt;
            // In size_t, so a length near 2^32 cannot wrap to a short skip.
            const std::size_t length = readBig32(m, at);
            at += 4 + ((length + 3) & ~std::size_t{3});
            break;
        }
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

std::size_t encodeOscFloat(std::span<std::byte> out, std::string_view address,
                           float value) noexcept PROJ_NONBLOCKING {
    const std::size_t addrBytes = padded(address.size());
    const std::size_t total = addrBytes + 4 + 4;
    if (address.empty() || total > out.size()) return 0;
    std::fill(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(total), std::byte{0});
    std::ranges::transform(address, out.begin(), [](char c) { return std::byte(c); });
    out[addrBytes] = std::byte{','};
    out[addrBytes + 1] = std::byte{'f'};
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (std::size_t i = 0; i < 4; ++i)
        out[addrBytes + 4 + i] = static_cast<std::byte>(bits >> (24U - 8U * i));
    return total;
}

} // namespace primitives
