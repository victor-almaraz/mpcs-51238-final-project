// Pipe references in tests, by kind and index. No dependency beyond
// metadata, so every module's tests can use them.

#pragma once

#include <cstdint>

#include <proj/metadata/graph.h>

namespace support {

constexpr metadata::pipeRef audio(std::int32_t index) {
    return {.kind = metadata::pipeKind::audio, .index = index};
}
constexpr metadata::pipeRef control(std::int32_t index) {
    return {.kind = metadata::pipeKind::control, .index = index};
}
constexpr metadata::pipeRef events(std::int32_t index) {
    return {.kind = metadata::pipeKind::event, .index = index};
}
constexpr metadata::pipeRef video(std::int32_t index) {
    return {.kind = metadata::pipeKind::video, .index = index};
}

}  // namespace support
