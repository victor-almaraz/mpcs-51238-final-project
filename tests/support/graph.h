// Builds graph descriptions in tests and renders them offline.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <proj/metadata/graph.h>
#include <proj/primitives/engine.h>

namespace support {

using metadata::pipeKind;
using metadata::pipeRef;

inline pipeRef audio(int index) { return {.kind = pipeKind::audio, .index = index}; }
inline pipeRef control(int index) { return {.kind = pipeKind::control, .index = index}; }
inline pipeRef events(int index) { return {.kind = pipeKind::event, .index = index}; }
inline pipeRef video(int index) { return {.kind = pipeKind::video, .index = index}; }

// A graph under construction. Nodes are added in execution order.
class graph {
public:
    graph& pipe(pipeRef r, int channels = 1, bool feedback = false) {
        spec.pipes.push_back({.pipe = r, .channels = channels, .feedback = feedback});
        return *this;
    }

    // Adds a node and returns it for parameters and ports.
    metadata::nodeSpec& node(std::string id, std::string type) {
        spec.nodes.push_back({.id = std::move(id),
                              .type = std::move(type),
                              .params = {},
                              .inputs = {},
                              .outputs = {},
                              .src = std::nullopt});
        return spec.nodes.back();
    }

    metadata::graphSpec spec;
};

// An engine that renders into owned output buffers.
class renderer {
public:
    explicit renderer(int outputs = 2, double sampleRate = 48000.0, int blockSize = 64)
        : engine({.audio = {.sampleRate = sampleRate, .blockSize = blockSize}, .pipes = {}}),
          buffers_(static_cast<std::size_t>(outputs)) {}

    // Builds @p g as plan @p id and plays it.
    void play(const metadata::graphSpec& g, std::int32_t id = 1, double fade = 0.0,
              primitives::quantization quant = primitives::quantization::none) {
        engine.play(engine.build(g, id), fade, quant);
    }

    // Renders @p n frames and appends them to the output history.
    void run(int n, const std::vector<std::vector<float>>& inputs = {}) {
        const auto count = static_cast<std::size_t>(n);
        std::vector<std::span<float>> outs;
        for (auto& b : buffers_) {
            b.resize(b.size() + count);
            outs.push_back(std::span(b).last(count));
        }
        const std::vector<std::span<const float>> ins(inputs.begin(), inputs.end());
        engine.process({.inputs = ins, .outputs = outs}, n, time_);
        time_ += n / engine.info().sampleRate;
    }

    const std::vector<float>& out(int channel = 0) const { return buffers_[static_cast<std::size_t>(channel)]; }

    primitives::engine engine;

private:
    std::vector<std::vector<float>> buffers_;
    double time_ = 0.0;
};

inline double rms(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
    to = std::min(to, x.size());
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) sum += static_cast<double>(x[i]) * x[i];
    return to > from ? std::sqrt(sum / static_cast<double>(to - from)) : 0.0;
}

inline double peak(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
    to = std::min(to, x.size());
    double m = 0.0;
    for (std::size_t i = from; i < to; ++i) m = std::max(m, std::abs(static_cast<double>(x[i])));
    return m;
}

// Upward zero crossings in [from, to): about one per period.
inline int crossings(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
    to = std::min(to, x.size());
    int n = 0;
    for (std::size_t i = from + 1; i < to; ++i) n += (x[i - 1] < 0.0f && x[i] >= 0.0f) ? 1 : 0;
    return n;
}

} // namespace support
