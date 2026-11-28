/// @file
/// Version of the primitive parameter tables. Client and server compare
/// it on connect (`protocol.md` §4.2).

#pragma once

#include <string_view>

namespace metadata {

/// Changes whenever a primitive's parameters change.
inline constexpr std::string_view version = "0.9.0";

}  // namespace metadata
