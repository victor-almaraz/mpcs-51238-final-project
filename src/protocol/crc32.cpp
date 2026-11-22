#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include <espresso/protocol/crc32.h>

namespace protocol {

namespace {

// Runs only in the compiler: the table is a constant of the program.
consteval std::array<std::uint32_t, 256> makeTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1u) : c >> 1u;
        table[i] = c;
    }
    return table;
}

constexpr auto table = makeTable();

// One CRC-32 step per byte of @p bytes, continuing from @p crc.
template <typename Bytes>
std::uint32_t update(const Bytes& bytes, std::uint32_t crc) noexcept {
    crc = ~crc;
    for (const auto b : bytes) crc = table[(crc ^ static_cast<std::uint8_t>(b)) & 0xFFu] ^ (crc >> 8u);
    return ~crc;
}

}  // namespace

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc) noexcept {
    return update(data, crc);
}

std::uint32_t crc32(std::string_view text, std::uint32_t crc) noexcept { return update(text, crc); }

} // namespace protocol
