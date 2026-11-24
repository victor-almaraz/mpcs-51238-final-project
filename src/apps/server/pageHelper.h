// The page helper, /espresso.js (io.md §12.9), and the page control
// library, /espresso-ui.js (ui.md §4): server/espresso.js and
// server/espresso-ui.js, embedded when the server is built.

#pragma once

#include <string_view>

namespace server {

/// The text of espresso.js.
std::string_view pageHelper() noexcept;

/// The text of espresso-ui.js.
std::string_view pageControls() noexcept;

}  // namespace server
