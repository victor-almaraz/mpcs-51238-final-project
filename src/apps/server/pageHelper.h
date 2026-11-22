// The page helper, /espresso.js (io.md §12.9): server/espresso.js,
// embedded when the server is built.

#pragma once

#include <string_view>

namespace server {

/// The text of espresso.js.
std::string_view pageHelper();

}  // namespace server
