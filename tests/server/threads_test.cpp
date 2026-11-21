// Determinism across thread counts (threads.md §1.3, §9.1): every example,
// and the load piece, renders the same samples on 1, 2, 4, and 8 threads.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/engine.h>

#include "offline.h"
#include "samples.h"
#include "support/unwrap.h"

namespace {

constexpr double rate = 48000.0;

// Every example's IR file, sorted.
std::vector<std::filesystem::path> examples() {
    std::vector<std::filesystem::path> out;
    for (const auto& e : std::filesystem::recursive_directory_iterator(
             std::filesystem::path(ESPRESSO_SOURCE_DIR) / "examples")) {
        if (e.path().string().ends_with(".ir.json")) out.push_back(e.path());
    }
    std::ranges::sort(out);
    return out;
}

metadata::graphSpec graphOf(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    const auto ir = ir::read(text.str());
    INFO(ir.text());
    REQUIRE(ir.ok());
    return support::unwrap(ir.graph);
}

// @p seconds of @p g rendered on @p threads threads, its samples loaded
// from @p root through @p cache, which renders of one piece share.
std::vector<std::vector<float>> render(const metadata::graphSpec& g, const std::filesystem::path& root,
                                       int threads, double seconds,
                                       const std::shared_ptr<server::sampleCache>& cache) {
    primitives::engine e(
        {.audio = {.sampleRate = rate, .blockSize = 256, .frameWidth = 64, .frameHeight = 36},
         .pipes = {.video = 16},
         .loadImage = nullptr,
         .loadSample = server::sampleLoaderFor(root, cache),
         .bufferMemory = primitives::bufferPool::defaultLimit,
         .profile = false,
         .threads = threads,
         .workerStart = nullptr});
    return server::renderOffline(e, e.build(g, 1), static_cast<std::size_t>(seconds * rate), 2);
}

}  // namespace

TEST_CASE("every example renders the same samples on 1, 2, 4, and 8 threads") {
    const auto paths = examples();
    REQUIRE(paths.size() >= 20);
    for (const auto& path : paths) {
        INFO(path.filename().string());
        const auto g = graphOf(path);
        const auto cache = std::make_shared<server::sampleCache>();
        const auto one = render(g, path.parent_path(), 1, 0.25, cache);
        for (const int threads : {2, 4, 8}) {
            INFO(threads << " threads");
            CHECK(render(g, path.parent_path(), threads, 0.25, cache) == one);
        }
    }
}

TEST_CASE("the load piece renders the same samples on 1 and 4 threads") {
    std::ifstream in(std::filesystem::path(ESPRESSO_SOURCE_DIR) / "tests" / "server" / "load" / "load.lisp");
    std::stringstream text;
    text << in.rdbuf();
    const auto compiled = interpreter::compile(text.str(), "load.lisp");
    REQUIRE(compiled.ok());
    const auto g = support::unwrap(compiled.graph);
    // Ten blocks: the first notes, each block 359 tasks.
    const auto cache = std::make_shared<server::sampleCache>();
    const auto one = render(g, ".", 1, 0.05, cache);
    CHECK(render(g, ".", 4, 0.05, cache) == one);
    CHECK(std::ranges::any_of(one[0], [](float x) { return x != 0.0f; }));
}
