// The IR validator's messages for a graph, one per line.

#pragma once

#include <string>

#include <proj/ir/ir.h>
#include <proj/metadata/graph.h>

namespace support {

inline std::string errors(const metadata::graphSpec& g) {
    std::string out;
    for (const auto& e : ir::validate(g)) out += e.text() + "\n";
    return out;
}

}  // namespace support
