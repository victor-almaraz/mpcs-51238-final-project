// wavetableOsc: band limiting at any frequency and morphing between tables
// (samples.md §9, §15.2).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <cstddef>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/engine.h>

#include "support/graph.h"

using Catch::Approx;
using primitives::decodedAudio;

namespace {

constexpr double rate = 48000.0;
constexpr std::size_t table = 2048;

// saw.wav: one sawtooth table. morph.wav: a sine, then its octave.
std::shared_ptr<const decodedAudio> tables(std::string_view path, double, std::string& error) {
    std::vector<float> x;
    if (path == "saw.wav") {
        for (std::size_t k = 0; k < table; ++k) {
            x.push_back(static_cast<float>((2.0 * static_cast<double>(k) / table) - 1.0));
        }
    } else if (path == "morph.wav") {
        for (const double harmonic : {1.0, 2.0}) {
            for (std::size_t k = 0; k < table; ++k) {
                x.push_back(static_cast<float>(
                    std::sin(2.0 * std::numbers::pi * harmonic * static_cast<double>(k) / table)));
            }
        }
    } else {
        error = "no such file";
        return nullptr;
    }
    return std::make_shared<decodedAudio>(decodedAudio{.channels = {std::move(x)}, .sampleRate = rate});
}

// Buffer 0, "tables", of the tables in @p file.
metadata::bufferDecl tablesOf(std::string file) {
    return {.buffer = {.index = 0},
            .id = "tables",
            .channels = 1,
            .seconds = std::nullopt,
            .file = std::move(file),
            .src = std::nullopt};
}

// wavetableOsc over @p file at @p freq and @p position, rendered for @p n
// samples.
std::vector<float> play(const std::string& file, double freq, double position, std::size_t n) {
    support::graph g;
    g.pipe(support::audio(0)).pipe(support::audio(1));
    g.spec.buffers = {tablesOf(file)};
    auto& osc = g.node("osc", "wavetableOsc");
    osc.params["buffer"] = metadata::bufferRef{.index = 0};
    osc.params["freq"] = freq;
    osc.params["position"] = position;
    osc.outputs["out"] = {support::audio(0)};
    // A quarter, so a saw's overshoot stays under the output limiter.
    auto& gain = g.node("quiet", "gain");
    gain.params["amount"] = 0.25;
    gain.inputs["in"] = {support::audio(0)};
    gain.outputs["out"] = {support::audio(1)};
    g.node("out", "audioOut").inputs["in"] = {support::audio(1)};
    support::renderer r({.audio = {.sampleRate = rate, .blockSize = 256},
                         .pipes = {},
                         .loadSample = tables,
                         .bufferMemory = primitives::bufferPool::defaultLimit},
                        1);
    r.engine.play(r.engine.build(g.spec, 1), 0.0, primitives::quantization::none);
    r.run(static_cast<int>(n));
    return r.out(0);
}

// The DFT magnitude of @p x at bin @p k of x.size().
double magnitude(const std::vector<float>& x, std::size_t k) {
    return support::amplitude(x, static_cast<double>(k) / static_cast<double>(x.size()));
}

}  // namespace

TEST_CASE("a band-limited saw has no partial above half the sample rate at any frequency (samples.md 9.2)") {
    // 4096 samples after a settling block; frequencies on DFT bins, so
    // every harmonic lands on a bin and anything between is aliasing.
    constexpr std::size_t n = 4096;
    for (const std::size_t bin : {6u, 60u, 300u, 900u, 1280u}) {  // about 70 Hz to 15 kHz
        const double freq = static_cast<double>(bin) * rate / n;
        INFO(freq << " Hz");
        auto x = play("saw.wav", freq, 0.0, n + 256);
        x.erase(x.begin(), x.begin() + 256);
        double harmonic = 0.0;
        double other = 0.0;
        double highest = 0.0;
        for (std::size_t k = 1; k < n / 2; ++k) {
            const double m = magnitude(x, k);
            if (k % bin == 0) {
                harmonic += m * m;
                if (m > 1e-4) highest = static_cast<double>(k) * rate / n;
            } else {
                other += m * m;
            }
        }
        CHECK(10.0 * std::log10(other / harmonic) < -60.0);
        CHECK(highest < rate / 2.0);
        // The fundamental keeps the saw's level: 2 / pi of 0.25.
        CHECK(magnitude(x, bin) == Approx(0.25 * 2.0 / std::numbers::pi).epsilon(0.02));
    }
}

TEST_CASE("wavetableOsc's position morphs between adjacent tables") {
    constexpr std::size_t n = 4096;
    constexpr std::size_t bin = 40;  // about 470 Hz
    const double freq = static_cast<double>(bin) * rate / n;
    for (const double position : {0.0, 0.5, 1.0}) {
        INFO(position);
        auto x = play("morph.wav", freq, position, n + 256);
        x.erase(x.begin(), x.begin() + 256);
        CHECK(magnitude(x, bin) == Approx(0.25 * (1.0 - position)).margin(1e-3));
        CHECK(magnitude(x, 2 * bin) == Approx(0.25 * position).margin(1e-3));
    }
}

TEST_CASE("a wavetable needs a buffer of at least one table") {
    support::graph g;
    g.pipe(support::audio(0));
    g.spec.buffers = {{.buffer = {.index = 0},
                       .id = "short",
                       .channels = 1,
                       .seconds = 0.01,
                       .file = std::nullopt,
                       .src = std::nullopt}};
    auto& osc = g.node("osc", "wavetableOsc");
    osc.params["buffer"] = metadata::bufferRef{.index = 0};
    osc.outputs["out"] = {support::audio(0)};
    primitives::engine e({.audio = {.sampleRate = rate, .blockSize = 256}, .pipes = {}});
    CHECK_THROWS_WITH(e.build(g.spec, 1), Catch::Matchers::ContainsSubstring("less than one table of 2048"));
}

TEST_CASE("oscillators of one buffer and table size share their tables (samples.md 9.2)") {
    // saw.wav is one table of 2048 frames: 11 versions of 4096 samples.
    constexpr std::size_t buffer = table * sizeof(float);
    constexpr std::size_t set = std::size_t{11} * 2 * table * sizeof(float);
    const auto oscillators = [](std::vector<double> sizes) {
        support::graph g;
        g.pipe(support::audio(0));
        g.spec.buffers = {tablesOf("saw.wav")};
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            auto& osc = g.node("osc" + std::to_string(i), "wavetableOsc");
            osc.params["buffer"] = metadata::bufferRef{.index = 0};
            osc.params["table-size"] = sizes[i];
            osc.outputs["out"] = {support::audio(0)};
        }
        return g.spec;
    };
    // Room for the buffer and one set of tables, not two.
    primitives::engine e({.audio = {.sampleRate = rate, .blockSize = 256},
                          .pipes = {},
                          .loadSample = tables,
                          .bufferMemory = buffer + set + (set / 2)});
    CHECK_NOTHROW(e.build(oscillators({2048, 2048, 2048}), 1));
    CHECK_THROWS_WITH(e.build(oscillators({2048, 1024}), 2),
                      Catch::Matchers::ContainsSubstring("--buffer-memory"));
}
