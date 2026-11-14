// Graph description helpers shared by the IR, the engine, and the compiler.

#include <proj/metadata/graph.h>

#include <algorithm>

namespace metadata {

std::string_view toString(pipeKind kind) noexcept {
    switch (kind) {
    case pipeKind::audio: return "audio";
    case pipeKind::control: return "control";
    case pipeKind::event: return "event";
    case pipeKind::video: return "video";
    }
    return "audio";
}

std::optional<pipeKind> parsePipeKind(std::string_view name) noexcept {
    for (auto k : {pipeKind::audio, pipeKind::control, pipeKind::event, pipeKind::video}) {
        if (toString(k) == name) return k;
    }
    return std::nullopt;
}

std::string toString(pipeRef ref) { return std::string(toString(ref.kind)) + " pipe " + std::to_string(ref.index); }

const pipeDecl* findPipe(const graphSpec& graph, pipeRef ref) noexcept {
    const auto it = std::find_if(graph.pipes.begin(), graph.pipes.end(), [&](const pipeDecl& d) { return d.pipe == ref; });
    return it == graph.pipes.end() ? nullptr : &*it;
}

std::vector<pipeRef> pipesRead(const nodeSpec& node) {
    std::vector<pipeRef> out;
    for (const auto& [port, refs] : node.inputs) out.insert(out.end(), refs.begin(), refs.end());
    for (const auto& [name, value] : node.params) {
        if (const auto* r = std::get_if<pipeRef>(&value)) out.push_back(*r);
    }
    return out;
}

std::vector<pipeRef> pipesWritten(const nodeSpec& node) {
    std::vector<pipeRef> out;
    for (const auto& [port, refs] : node.outputs) out.insert(out.end(), refs.begin(), refs.end());
    return out;
}

} // namespace metadata
