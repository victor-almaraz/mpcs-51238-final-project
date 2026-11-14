// Event construction, MIDI conversion, and OSC helpers (primitives.md §5.4).

#include <proj/primitives/event.h>

#include <algorithm>
#include <bit>
#include <array>
#include <iterator>

namespace primitives {

namespace {

template <typename T>
void put(event& e, std::size_t at, T value) noexcept {
    const auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    std::copy(bytes.begin(), bytes.end(), e.payload.begin() + static_cast<std::ptrdiff_t>(at));
}

template <typename T>
T get(const event& e, std::size_t at) noexcept {
    std::array<std::byte, sizeof(T)> bytes{};
    std::copy_n(e.payload.begin() + static_cast<std::ptrdiff_t>(at), sizeof(T), bytes.begin());
    return std::bit_cast<T>(bytes);
}

int clamp7(int v) noexcept { return std::clamp(v, 0, 127); }

// OSC strings are null-terminated and padded to 4 bytes.
std::size_t padded(std::size_t n) noexcept { return (n + 4) & ~std::size_t{3}; }

// Big-endian integers starting at @p at; the caller checks the length.
std::uint32_t readBig32(std::span<const std::byte> m, std::size_t at) noexcept {
    std::uint32_t v = 0;
    for (std::size_t k = 0; k < 4; ++k) v = (v << 8) | std::to_integer<std::uint32_t>(m[at + k]);
    return v;
}

std::uint64_t readBig64(std::span<const std::byte> m, std::size_t at) noexcept {
    return (std::uint64_t{readBig32(m, at)} << 32) | readBig32(m, at + 4);
}

// Length of the string starting at @p at, or nothing when unterminated.
std::optional<std::size_t> stringLength(std::span<const std::byte> m, std::size_t at) noexcept {
    if (at >= m.size()) return std::nullopt;
    const auto rest = m.subspan(at);
    const auto end = std::find(rest.begin(), rest.end(), std::byte{0});
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

event midiEvent(std::uint32_t offset, eventType type, int ch, int d1, int d2) noexcept {
    event e;
    e.sampleOffset = offset;
    e.type = type;
    e.size = 3;
    e.payload[0] = static_cast<std::byte>(std::clamp(ch, 0, 15));
    e.payload[1] = static_cast<std::byte>(clamp7(d1));
    e.payload[2] = static_cast<std::byte>(clamp7(d2));
    return e;
}

event noteOn(std::uint32_t offset, int ch, int note, int velocity) noexcept {
    return midiEvent(offset, eventType::noteOn, ch, note, velocity);
}

event noteOff(std::uint32_t offset, int ch, int note, int velocity) noexcept {
    return midiEvent(offset, eventType::noteOff, ch, note, velocity);
}

event pitchBend(std::uint32_t offset, int ch, int value) noexcept {
    const int v = std::clamp(value, 0, 16383);
    return midiEvent(offset, eventType::pitchBend, ch, v & 0x7f, v >> 7);
}

event trigger(std::uint32_t offset, float value) noexcept {
    event e;
    e.sampleOffset = offset;
    e.type = eventType::trigger;
    e.size = sizeof value;
    put(e, 0, value);
    return e;
}

event valueEvent(std::uint32_t offset, float value) noexcept {
    event e = trigger(offset, value);
    e.type = eventType::value;
    return e;
}

event oscEvent(std::uint32_t offset, poolRef data) noexcept {
    event e;
    e.sampleOffset = offset;
    e.type = eventType::osc;
    e.size = sizeof data;
    put(e, 0, data);
    return e;
}

int channel(const event& e) noexcept { return std::to_integer<int>(e.payload[0]); }
int data1(const event& e) noexcept { return std::to_integer<int>(e.payload[1]); }
int data2(const event& e) noexcept { return std::to_integer<int>(e.payload[2]); }
int bendValue(const event& e) noexcept { return data1(e) | (data2(e) << 7); }

float number(const event& e) noexcept {
    if (e.type == eventType::trigger || e.type == eventType::value) return get<float>(e, 0);
    if (e.type == eventType::pitchBend) return static_cast<float>(bendValue(e)) / 16383.0f;
    if (e.type == eventType::programChange || e.type == eventType::channelPressure) {
        return static_cast<float>(data1(e)) / 127.0f;
    }
    if (isMidi(e.type)) return static_cast<float>(data2(e)) / 127.0f;
    return 0.0f;
}

poolRef oscData(const event& e) noexcept { return get<poolRef>(e, 0); }

std::optional<event> fromMidi(std::span<const std::uint8_t> bytes, std::uint32_t offset) noexcept {
    if (bytes.empty() || bytes[0] < 0x80 || bytes[0] >= 0xf0) return std::nullopt;
    const int status = bytes[0] & 0xf0;
    const int ch = bytes[0] & 0x0f;
    const std::size_t need = (status == 0xc0 || status == 0xd0) ? 2 : 3;
    if (bytes.size() < need) return std::nullopt;
    const int d1 = bytes[1] & 0x7f;
    const int d2 = need == 3 ? (bytes[2] & 0x7f) : 0;
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

int toMidi(const event& e, std::array<std::uint8_t, 3>& out) noexcept {
    int status = 0;
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
    out = {static_cast<std::uint8_t>(status | channel(e)), static_cast<std::uint8_t>(data1(e)),
           static_cast<std::uint8_t>(size == 3 ? data2(e) : 0)};
    return size;
}

std::string oscAddress(std::span<const std::byte> m) {
    const auto n = addressLength(m);
    std::string out;
    if (n) std::transform(m.begin(), m.begin() + static_cast<std::ptrdiff_t>(*n), std::back_inserter(out),
                          [](std::byte b) { return static_cast<char>(b); });
    return out;
}

bool oscAddressIs(std::span<const std::byte> m, std::string_view address) noexcept {
    const auto n = addressLength(m);
    return n && *n == address.size() &&
           std::equal(address.begin(), address.end(), m.begin(), [](char c, std::byte b) { return std::byte(c) == b; });
}

std::optional<float> oscFirstNumber(std::span<const std::byte> m) noexcept {
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
        case 'b':
            if (at + 4 > m.size()) return std::nullopt;
            at += 4 + ((readBig32(m, at) + 3) & ~3u);
            break;
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

std::size_t encodeOscFloat(std::span<std::byte> out, std::string_view address, float value) noexcept {
    const std::size_t addrBytes = padded(address.size());
    const std::size_t total = addrBytes + 4 + 4;
    if (address.empty() || total > out.size()) return 0;
    std::fill(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(total), std::byte{0});
    std::transform(address.begin(), address.end(), out.begin(), [](char c) { return std::byte(c); });
    out[addrBytes] = std::byte{','};
    out[addrBytes + 1] = std::byte{'f'};
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (int i = 0; i < 4; ++i) {
        out[addrBytes + 4 + static_cast<std::size_t>(i)] = static_cast<std::byte>(bits >> (24 - 8 * i));
    }
    return total;
}

} // namespace primitives
