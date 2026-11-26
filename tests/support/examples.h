// Engines to build the examples, a rig that plays one,
// and the load piece, for the server's tests (threads.md §9).

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/io.h>

#include "samples.h"
#include "support/compile.h"
#include "support/files.h"
#include "support/graph.h"
#include "support/unwrap.h"

namespace support {

/// One sample cache for the test process, so each file decodes once.
inline const std::shared_ptr<server::sampleCache>& sharedSamples() {
    static const auto cache = std::make_shared<server::sampleCache>();
    return cache;
}

/// An engine's options for the examples: 48 kHz, 256-sample blocks, the
/// server's pipe table, samples from @p root, and @p threads threads.
inline primitives::engineOptions exampleOptions(const std::filesystem::path& root, int threads = 1) {
    return {.audio = {.sampleRate = 48000.0, .blockSize = 256},
            .pipes = {},
            .loadSample = server::sampleLoaderFor(root, sharedSamples()),
            .bufferMemory = primitives::bufferPool::defaultLimit,
            .profile = false,
            .threads = threads};
}

/// The folder of example @p name, `examples/NAME`.
inline std::filesystem::path exampleFolder(const std::string& name) {
    return std::filesystem::path(ESPRESSO_SOURCE_DIR) / "examples" / name;
}

/// Plays examples/NAME/NAME.lisp as plan 1 on an engine of
/// exampleOptions(), its samples from the example's folder, with both
/// device inputs fed the same signal, and records two outputs.
class examplePiece {
public:
    explicit examplePiece(const std::string& name)
        : r_(exampleOptions(exampleFolder(name)), 2),
          graph_(pieceGraph(exampleFolder(name) / (name + ".lisp"))) {
        r_.play(graph_, 1);
    }

    /// Renders @p frames more samples, the device inputs from @p input at
    /// the same time, silent past its end.
    void run(std::size_t frames, const std::vector<float>& input = {}) {
        std::vector<float> in(frames, 0.0f);
        for (std::size_t i = 0; i < frames && clock_ + i < input.size(); ++i) in[i] = input[clock_ + i];
        r_.run(static_cast<int>(frames), {in, in});
        clock_ += frames;
    }

    /// Samples rendered so far.
    std::size_t clock() const noexcept { return clock_; }
    /// Output @p channel, from the first sample rendered.
    const std::vector<float>& out(int channel = 0) const { return r_.out(channel); }
    /// The engine's I/O hub, for feeding sources and draining sinks.
    primitives::ioHub& io() { return r_.engine.io(); }
    /// The example's graph.
    const metadata::graphSpec& graph() const noexcept { return graph_; }

    /// A MIDI message from a keyboard.
    void midi(std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
        io().deliverMidi("Keyboard", support::midi(status, d1, d2));
    }

private:
    renderer r_;
    const metadata::graphSpec& graph_;  // compiled once per process
    std::size_t clock_ = 0;
};

/// The load piece of the v2 load test (`examples/straintest`), with
/// @p layers layers a voice when given and the piece's own count otherwise;
/// each count compiles once.
inline const metadata::graphSpec& loadPiece(std::optional<int> layers = std::nullopt) {
    static std::map<std::optional<int>, metadata::graphSpec> compiled;
    if (const auto it = compiled.find(layers); it != compiled.end()) return it->second;
    auto source = readText(exampleFolder("straintest") / "straintest.lisp");
    if (layers) {
        constexpr std::string_view line = "(define layers 8)";
        const auto at = source.find(line);
        REQUIRE(at != std::string::npos);
        source.replace(at, line.size(), "(define layers " + std::to_string(*layers) + ")");
    }
    const auto result = interpreter::compile(source, "straintest.lisp");
    INFO(result.text());
    REQUIRE(result.ok());
    auto g = unwrap(result.graph);
    REQUIRE(ir::validate(g).empty());
    return compiled.emplace(layers, std::move(g)).first->second;
}

}  // namespace support
