/// @file
/// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320), used to check
/// assembled IR (`protocol.md` §6.5).

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace protocol {

/// CRC-32 of @p data. Pass a previous result as @p crc to continue a
/// running checksum.
std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0) noexcept;

/// CRC-32 of the bytes of @p text. Pass a previous result as @p crc to
/// continue a running checksum.
std::uint32_t crc32(std::string_view text, std::uint32_t crc = 0) noexcept;

}  // namespace protocol
