/// @file
/// UTF-8 helpers for the editor. Columns are counted in code points
/// (`tui.md` §5.2).

#pragma once

#include <cstddef>
#include <string_view>

/// UTF-8 decoding and code point counting for the editor's buffers.
namespace tui::utf8 {

/// True when @p byte is a continuation byte (10xxxxxx).
constexpr bool isContinuation(unsigned char byte) noexcept { return (byte & 0xC0u) == 0x80u; }

/// Returns true when @p text is well-formed UTF-8 (no overlong forms,
/// surrogates, or code points above U+10FFFF).
bool isValid(std::string_view text) noexcept;

/// Number of code points in @p text.
std::size_t length(std::string_view text) noexcept;

/// Byte offset of code point @p index in @p text, clamped to its size.
std::size_t byteOffset(std::string_view text, std::size_t index) noexcept;

/// Length in bytes of the code point that starts with @p lead.
std::size_t sequenceLength(unsigned char lead) noexcept;

} // namespace tui::utf8
