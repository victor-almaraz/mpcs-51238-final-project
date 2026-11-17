// Graph description helpers shared by the IR, the engine, and the compiler.

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/metadata/graph.h>

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

optionalRef<pipeDecl> findPipe(const graphSpec& graph, pipeRef ref) noexcept {
    const auto it = std::ranges::find_if(graph.pipes, [&](const pipeDecl& d) { return d.pipe == ref; });
    if (it == graph.pipes.end()) return std::nullopt;
    return std::cref(*it);
}

std::vector<pipeRef> pipesRead(const nodeSpec& node) {
    std::vector<pipeRef> out;
    for (const auto& [port, refs] : node.inputs) out.insert(out.end(), refs.begin(), refs.end());
    for (const auto& [name, value] : node.params) {
        if (const auto r = as<pipeRef>(value)) out.push_back(*r);
    }
    return out;
}

std::vector<pipeRef> pipesWritten(const nodeSpec& node) {
    std::vector<pipeRef> out;
    for (const auto& [port, refs] : node.outputs) out.insert(out.end(), refs.begin(), refs.end());
    return out;
}

std::map<pipeRef, std::vector<std::size_t>> writersByPipe(const graphSpec& graph, bool skipFeedback) {
    std::set<pipeRef> feedback;
    if (skipFeedback) {
        for (const auto& d : graph.pipes) {
            if (d.feedback) feedback.insert(d.pipe);
        }
    }
    std::map<pipeRef, std::vector<std::size_t>> writers;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        for (const auto r : pipesWritten(graph.nodes[i])) {
            if (!feedback.contains(r)) writers[r].push_back(i);
        }
    }
    return writers;
}

} // namespace metadata
