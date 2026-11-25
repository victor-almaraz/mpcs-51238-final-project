#include <cstddef>
#include <cstdint>
#include <string_view>

#include <espresso/workspace/utf8.h>

namespace workspace::utf8 {

bool isValid(std::string_view text) noexcept {
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const auto b0 = static_cast<unsigned char>(text[i]);
        if (b0 < 0x80u) {
            ++i;
            continue;
        }
        std::size_t len = 0;
        std::uint32_t cp = 0;
        std::uint32_t min = 0;
        if ((b0 & 0xE0u) == 0xC0u) {
            len = 2;
            cp = b0 & 0x1Fu;
            min = 0x80;
        } else if ((b0 & 0xF0u) == 0xE0u) {
            len = 3;
            cp = b0 & 0x0Fu;
            min = 0x800;
        } else if ((b0 & 0xF8u) == 0xF0u) {
            len = 4;
            cp = b0 & 0x07u;
            min = 0x10000;
        } else {
            return false;
        }
        if (i + len > n) return false;
        for (std::size_t k = 1; k < len; ++k) {
            const auto b = static_cast<unsigned char>(text[i + k]);
            if (!isContinuation(b)) return false;
            cp = (cp << 6u) | (b & 0x3Fu);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

std::size_t length(std::string_view text) noexcept {
    std::size_t count = 0;
    for (char c : text) {
        if (!isContinuation(static_cast<unsigned char>(c))) ++count;
    }
    return count;
}

std::size_t byteOffset(std::string_view text, std::size_t index) noexcept {
    std::size_t i = 0;
    while (i < text.size() && index > 0) {
        i += sequenceLength(static_cast<unsigned char>(text[i]));
        --index;
    }
    return i < text.size() ? i : text.size();
}

std::size_t sequenceLength(unsigned char lead) noexcept {
    if (lead < 0x80u) return 1;
    if ((lead & 0xE0u) == 0xC0u) return 2;
    if ((lead & 0xF0u) == 0xE0u) return 3;
    if ((lead & 0xF8u) == 0xF0u) return 4;
    return 1;  // stray byte: treat as one unit
}

}  // namespace workspace::utf8
