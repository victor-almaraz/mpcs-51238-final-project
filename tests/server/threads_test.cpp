// Determinism across thread counts (threads.md §1.3, §9.1): every example,
// and the load piece, renders the same samples on 1, 2, 4, and 8 threads.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/engine.h>

#include "offline.h"
#include "support/examples.h"

namespace {

constexpr double rate = 48000.0;

// @p seconds of @p g rendered on @p threads threads, its samples loaded
// from @p root.
std::vector<std::vector<float>> render(const metadata::graphSpec& g, const std::filesystem::path& root,
                                       int threads, double seconds) {
    primitives::engine e(support::exampleOptions(root, threads));
    return server::renderOffline(e, e.build(g, 1), static_cast<std::size_t>(seconds * rate), 2);
}

}  // namespace

TEST_CASE("every example renders the same samples on 1, 2, 4, and 8 threads") {
    const auto paths = support::exampleIrFiles();
    REQUIRE(paths.size() >= 20);
    for (const auto& path : paths) {
        INFO(path.filename().string());
        const auto g = support::irGraph(path);
        const auto one = render(g, path.parent_path(), 1, 0.25);
        for (const int threads : {2, 4, 8}) {
            INFO(threads << " threads");
            CHECK(render(g, path.parent_path(), threads, 0.25) == one);
        }
    }
}

TEST_CASE("the load piece renders the same samples on 1 and 4 threads") {
    // Ten blocks: the first notes, each block 359 tasks. The load piece
    // needs the server's pipe table, not the examples'.
    auto render = [](int threads) {
        primitives::engine e(
            {.audio = {.sampleRate = rate, .blockSize = 256}, .pipes = {}, .threads = threads});
        return server::renderOffline(e, e.build(support::loadPiece(), 1),
                                     static_cast<std::size_t>(0.05 * rate), 2);
    };
    const auto one = render(1);
    CHECK(render(4) == one);
    CHECK(std::ranges::any_of(one[0], [](float x) { return x != 0.0f; }));
}
