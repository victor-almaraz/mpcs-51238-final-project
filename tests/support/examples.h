// The examples' IR files, engines to build them, and the load piece, for
// the server's tests (threads.md §9).

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/engine.h>

#include "samples.h"
#include "support/unwrap.h"

namespace support {

/// Every example's IR file, sorted.
inline std::vector<std::filesystem::path> exampleIrFiles() {
    std::vector<std::filesystem::path> out;
    for (const auto& e : std::filesystem::recursive_directory_iterator(
             std::filesystem::path(ESPRESSO_SOURCE_DIR) / "examples")) {
        if (e.path().string().ends_with(".ir.json")) out.push_back(e.path());
    }
    std::ranges::sort(out);
    return out;
}

/// The graph in the IR file @p path; fails the test when it does not read.
inline metadata::graphSpec irGraph(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    const auto ir = ir::read(text.str());
    INFO(ir.text());
    REQUIRE(ir.ok());
    return unwrap(ir.graph);
}

/// One sample cache for the test process, so each file decodes once.
inline const std::shared_ptr<server::sampleCache>& sharedSamples() {
    static const auto cache = std::make_shared<server::sampleCache>();
    return cache;
}

/// An engine's options for the examples: 48 kHz, 256-sample blocks, the
/// server's pipe table with its 16 video pipes at a small frame size,
/// samples from @p root, and @p threads threads.
inline primitives::engineOptions exampleOptions(const std::filesystem::path& root, int threads = 1) {
    return {.audio = {.sampleRate = 48000.0, .blockSize = 256, .frameWidth = 64, .frameHeight = 36},
            .pipes = {.video = 16},
            .loadImage = nullptr,
            .loadSample = server::sampleLoaderFor(root, sharedSamples()),
            .bufferMemory = primitives::bufferPool::defaultLimit,
            .profile = false,
            .threads = threads};
}

/// The load piece of the v2 load test (`examples/straintest`), with
/// @p layers layers a voice when given and the piece's own count otherwise;
/// each count compiles once.
inline const metadata::graphSpec& loadPiece(std::optional<int> layers = std::nullopt) {
    static std::map<std::optional<int>, metadata::graphSpec> compiled;
    if (const auto it = compiled.find(layers); it != compiled.end()) return it->second;
    std::ifstream in(std::filesystem::path(ESPRESSO_SOURCE_DIR) / "examples" / "straintest" /
                     "straintest.lisp");
    std::stringstream text;
    text << in.rdbuf();
    auto source = text.str();
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
