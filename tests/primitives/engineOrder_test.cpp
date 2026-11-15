// The merged execution order of a crossfade (primitives.md §9.3.1), read
// from the engine's internal transition builder.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <proj/primitives/engine.h>

#include "../../src/primitives/transition.h"
#include "support/graph.h"

using namespace support;
using namespace primitives;

namespace {

void gain(graph& g, const std::string& id, int in, int out) {
    auto& n = g.node(id, "gain");
    n.inputs["in"] = {audio(in)};
    n.outputs["out"] = {audio(out)};
}

}  // namespace

TEST_CASE("a crossfade cycle is cut at a retiring node on the cycle") {
    // Pipes: 0 = A, 1 = B, 2 = C, 3 = E. Each plan alone is acyclic; merged,
    // o1-i1 (through B and A) and o2-i2 (through E and C) are cycles, and x
    // joins them: it reads A from the first and writes C into the second.
    graph outgoing, incoming;
    for (graph& g : {std::ref(outgoing), std::ref(incoming)}) {
        for (int i = 0; i < 4; ++i) g.pipe(audio(i));
    }
    gain(outgoing, "x", 0, 2);
    gain(outgoing, "o1", 0, 1);
    gain(outgoing, "o2", 2, 3);
    gain(incoming, "i1", 1, 0);
    gain(incoming, "i2", 3, 2);
    engine e({.audio = {.sampleRate = 48000.0, .blockSize = 64}, .pipes = {}});
    const auto t = makeTransition(e.build(incoming.spec, 2), e.build(outgoing.spec, 1), 0.05,
                                  quantization::none, 48000.0);

    std::vector<std::string> ids;
    for (const auto& entry : t->order) ids.push_back(entry.node->id());
    REQUIRE(ids.size() == 5);
    const auto at = [&](std::string_view id) { return std::ranges::find(ids, id) - ids.begin(); };
    // x is not on a cycle, so it is never cut: it runs after i1 writes A.
    CHECK(at("o1") < at("i1"));
    CHECK(at("i1") < at("x"));
    CHECK(at("x") < at("o2"));
    CHECK(at("o2") < at("i2"));
}
