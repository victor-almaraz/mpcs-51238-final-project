// Builds graph descriptions in tests and renders them offline.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/engine.h>

#include "support/pipes.h"  // IWYU pragma: export

namespace support {

using metadata::pipeKind;
using metadata::pipeRef;


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

    // An engine of @p options, such as one with a sample loader, with
    // @p outputs outputs.
    renderer(primitives::engineOptions options, int outputs)
        : engine(std::move(options)), buffers_(static_cast<std::size_t>(outputs)) {}

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

// A block size at which a sineOsc is a task of its own: its estimated
// cost, 21 ns a sample (src/primitives/costs.cpp), reaches the least cost
// of a task, 20 us (threads.md §5.2), at 1024 samples. A small graph with
// two sines that do not depend on each other then has a parallel
// schedule, which an engine of more than one thread runs on its workers.
// Tests may still process 64 samples a call.
inline constexpr int taskBlockSize = 1024;

// Options for an engine of @p threads threads, with blocks of
// taskBlockSize samples and four audio pipes, which take 64 KB each at
// that size.
inline primitives::engineOptions taskBlockOptions(int threads) {
    return {.audio = {.sampleRate = 48000.0, .blockSize = taskBlockSize},
            .pipes = {.audio = 4},
            .threads = threads};
}

inline double rms(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
    to = std::min(to, x.size());
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) sum += static_cast<double>(x[i]) * x[i];
    return to > from ? std::sqrt(sum / static_cast<double>(to - from)) : 0.0;
}

// The amplitude of the component of @p x at @p cycles a sample, over a
// whole number of its cycles: a DFT at one frequency.
inline double amplitude(std::span<const float> x, double cycles) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t k = 0; k < x.size(); ++k) {
        const double w = 2.0 * std::numbers::pi * cycles * static_cast<double>(k);
        re += static_cast<double>(x[k]) * std::cos(w);
        im -= static_cast<double>(x[k]) * std::sin(w);
    }
    return x.empty() ? 0.0 : 2.0 * std::hypot(re, im) / static_cast<double>(x.size());
}

inline double peak(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
    to = std::min(to, x.size());
    double m = 0.0;
    for (std::size_t i = from; i < to; ++i) m = std::max(m, std::abs(static_cast<double>(x[i])));
    return m;
}

// A background of @p color on video pipe 0, shown by the visual layer.
inline metadata::graphSpec shown(std::vector<double> color) {
    graph g;
    g.pipe(video(0));
    auto& bg = g.node("bg", "background");
    bg.params["color"] = std::move(color);
    bg.outputs["out"] = {video(0)};
    g.node("show", "videoOut").inputs["in"] = {video(0)};
    return g.spec;
}

// Upward zero crossings in [from, to): about one per period.
inline int crossings(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
    to = std::min(to, x.size());
    int n = 0;
    for (std::size_t i = from + 1; i < to; ++i) n += (x[i - 1] < 0.0f && x[i] >= 0.0f) ? 1 : 0;
    return n;
}

// The pitch of @p x between @p from and @p to, 50 Hz to 2 kHz, by
// autocorrelation: the shortest lag whose correlation is within 10% of the
// strongest, so a tone rich in partials or effects is not read an octave
// off, as counting crossings would.
inline double pitch(const std::vector<float>& x, std::size_t from, std::size_t to, int sampleRate = 48000) {
    to = std::min(to, x.size());
    const auto shortest = static_cast<std::size_t>(sampleRate / 2000);
    const auto longest = static_cast<std::size_t>(sampleRate / 50);
    std::vector<double> r(longest + 1, 0.0);
    for (std::size_t lag = shortest; lag <= longest; ++lag) {
        for (std::size_t i = from; i + lag < to; ++i) r[lag] += static_cast<double>(x[i]) * x[i + lag];
    }
    const double best = *std::ranges::max_element(r);
    for (std::size_t lag = shortest + 1; lag < longest; ++lag) {
        if (r[lag] >= 0.9 * best && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1])
            return static_cast<double>(sampleRate) / static_cast<double>(lag);
    }
    return 0.0;
}

// @p seconds of a sine at @p freq and @p amplitude.
inline std::vector<float> sine(double freq, double seconds, double amplitude = 0.5, int sampleRate = 48000) {
    std::vector<float> x(static_cast<std::size_t>(seconds * sampleRate));
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * freq * static_cast<double>(i) / sampleRate));
    }
    return x;
}

} // namespace support
