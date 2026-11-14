#include <proj/tui/keyNames.h>

#include <array>

#include <proj/tui/utf8.h>

namespace tui {

namespace {

// Name of a single C0 control byte or DEL.
std::string controlName(unsigned char c) {
    switch (c) {
    case 0x00: return "C-SPC";
    case 0x09: return "TAB";
    case 0x0A:
    case 0x0D: return "RET";
    case 0x1B: return "ESC";
    case 0x1C: return "C-\\";
    case 0x1D: return "C-]";
    case 0x1E: return "C-^";
    case 0x1F: return "C-_";
    case 0x7F: return "DEL";
    default: break;
    }
    if (c >= 0x01 && c <= 0x1A) return std::string("C-") + static_cast<char>('a' + c - 1);
    return {};
}

// Name of a key without escape prefix: a control byte, space, or a
// printable character.
std::string plainName(std::string_view input) {
    if (input.empty()) return {};
    const auto c = static_cast<unsigned char>(input[0]);
    if (input.size() == 1 && (c < 0x20u || c == 0x7Fu)) return controlName(c);
    if (input == " ") return "SPC";
    if (utf8::sequenceLength(c) == input.size() && utf8::isValid(input)) return std::string(input);
    return {};
}

// Modifier prefix for an xterm modifier parameter (1 + bitmask of
// Shift=1, Alt=2, Ctrl=4).
std::string modifierPrefix(int parameter) {
    const int bits = parameter - 1;
    std::string out;
    if (bits & 4) out += "C-";
    if (bits & 2) out += "M-";
    if (bits & 1) out += "S-";
    return out;
}

std::string_view finalKeyName(char final) {
    switch (final) {
    case 'A': return "<up>";
    case 'B': return "<down>";
    case 'C': return "<right>";
    case 'D': return "<left>";
    case 'H': return "<home>";
    case 'F': return "<end>";
    case 'P': return "<f1>";
    case 'Q': return "<f2>";
    case 'R': return "<f3>";
    case 'S': return "<f4>";
    default: return {};
    }
}

std::string_view tildeKeyName(int code) {
    switch (code) {
    case 1: case 7: return "<home>";
    case 2: return "<insert>";
    case 3: return "<delete>";
    case 4: case 8: return "<end>";
    case 5: return "<prior>";
    case 6: return "<next>";
    case 11: return "<f1>";
    case 12: return "<f2>";
    case 13: return "<f3>";
    case 14: return "<f4>";
    case 15: return "<f5>";
    case 17: return "<f6>";
    case 18: return "<f7>";
    case 19: return "<f8>";
    case 20: return "<f9>";
    case 21: return "<f10>";
    case 23: return "<f11>";
    case 24: return "<f12>";
    default: return {};
    }
}

// Parses the parameters of a CSI sequence body like "1;5" into at most
// two integers. Returns false on anything else.
bool parseParameters(std::string_view body, int& first, int& second) {
    std::array<int, 2> values{1, 1};
    std::size_t which = 0;
    bool seen = false;
    for (char c : body) {
        if (c >= '0' && c <= '9') {
            if (!seen) values[which] = 0;
            values[which] = values[which] * 10 + (c - '0');
            seen = true;
        } else if (c == ';' && which == 0) {
            which = 1;
            seen = false;
        } else {
            return false;
        }
    }
    first = values[0];
    second = values[1];
    return true;
}

std::string csiName(std::string_view seq) {
    // seq is everything after "ESC [".
    if (seq.empty()) return {};
    const char final = seq.back();
    const std::string_view body = seq.substr(0, seq.size() - 1);
    if (final == 'Z' && body.empty()) return "S-TAB";
    if (!body.empty() && body[0] == '<') return {};  // SGR mouse report
    int first = 1;
    int second = 1;
    if (!parseParameters(body, first, second)) return {};
    if (final == '~') {
        const std::string_view name = tildeKeyName(first);
        return name.empty() ? std::string() : modifierPrefix(second) + std::string(name);
    }
    const std::string_view name = finalKeyName(final);
    if (name.empty()) return {};
    return modifierPrefix(body.empty() ? 1 : second) + std::string(name);
}

} // namespace

std::string keyNameFromInput(std::string_view input) {
    if (input.empty()) return {};
    if (input[0] != '\x1B' || input.size() == 1) return plainName(input);

    const std::string_view rest = input.substr(1);
    if (rest[0] == '[' && rest.size() >= 2) return csiName(rest.substr(1));
    if (rest[0] == 'O' && rest.size() == 2) {
        const std::string_view name = finalKeyName(rest[1]);
        return std::string(name);
    }

    // ESC followed by a key is Meta plus that key.
    const std::string inner = plainName(rest);
    if (inner.empty() || inner == "ESC") return {};
    if (inner.starts_with("C-")) return "C-M-" + inner.substr(2);
    return "M-" + inner;
}

} // namespace tui
