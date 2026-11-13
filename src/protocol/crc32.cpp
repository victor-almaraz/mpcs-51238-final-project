#include <proj/protocol/crc32.h>

#include <array>

namespace protocol {

namespace {

constexpr std::array<std::uint32_t, 256> makeTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    return table;
}

constexpr auto table = makeTable();

} // namespace

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc) noexcept {
    crc = ~crc;
    for (std::uint8_t b : data) crc = table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

std::uint32_t crc32(std::string_view text) noexcept {
    return crc32(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

} // namespace protocol
