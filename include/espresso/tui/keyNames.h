/// @file
/// Translation from raw terminal input to key names in the notation of
/// keymap.h. Has no terminal dependency.

#pragma once

#include <string>
#include <string_view>

namespace tui {

/// Returns the key name for one terminal input event, such as `"C-x"`
/// for byte 0x18, `"S-<left>"` for `ESC [ 1 ; 2 D`, or `"é"` for a
/// printable character. Returns an empty string for input that is not a
/// key (mouse reports, terminal replies, unknown sequences).
std::string keyNameFromInput(std::string_view input);

}  // namespace tui
