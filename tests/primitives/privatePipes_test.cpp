// Private pipes for writers of one audio pipe, and the summing nodes that
// add them (threads.md §4.2).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/plan.h>

#include "support/graph.h"
#include "support/pipes.h"

using Catch::Approx;
using namespace support;

namespace {

// A constant of @p level on audio pipe @p pipeIndex.
void constant(graph& g, const std::string& id, int pipeIndex, double level) {
    auto& n = g.node(id, "lfo");
    n.params = {{"depth", 0.0}, {"offset", level}};
    n.outputs["out"] = {audio(pipeIndex)};
}

// Constants of @p levels into audio pipe 0, which device channel 0 plays.
metadata::graphSpec writers(const std::vector<std::pair<std::string, double>>& levels) {
    graph g;
    g.pipe(audio(0));
    for (const auto& [id, level] : levels) constant(g, id, 0, level);
    auto& out = g.node("out", "audioOut");
    out.inputs["in"] = {audio(0)};
    return g.spec;
}

std::vector<std::uint32_t> listed(const primitives::plan& p, std::size_t i) {
    const auto l = p.schedule.dependencies.of(i);
    return {l.begin(), l.end()};
}

}  // namespace

TEST_CASE(
    "writers of one audio pipe after the first write private pipes, which a summing node adds in order") {
    renderer r(1, 48000.0, 64);
    const auto p = r.engine.build(writers({{"a", 0.1}, {"b", 0.2}, {"c", 0.3}}), 1);
    REQUIRE(p->nodes.size() == 5);
    CHECK(p->nodes[3]->type() == "sum");  // right after the last writer
    CHECK(p->info[3].sums());
    CHECK(p->privatePipes.size() == 2);
    CHECK(p->info[0].privateWrites.empty());  // the first writer writes the pipe itself
    CHECK(p->info[1].privateWrites.size() == 1);
    CHECK(p->info[2].privateWrites.size() == 1);

    // The writers are independent; the sum follows all three, the sink the sum.
    CHECK(listed(*p, 1).empty());
    CHECK(listed(*p, 2).empty());
    CHECK(listed(*p, 3) == std::vector<std::uint32_t>{0, 1, 2});
    CHECK(listed(*p, 4) == std::vector<std::uint32_t>{3});

    r.engine.play(p, 0.0, primitives::quantization::none);
    r.run(128);
    // The bits the three writers would have left in the pipe themselves.
    const float expected = (0.1f + 0.2f) + 0.3f;
    CHECK(r.out().back() == expected);
}

TEST_CASE("an audio pipe with one writer, and other kinds of pipe, get no private pipe") {
    renderer r(1, 48000.0, 64);
    graph g;
    g.pipe(audio(0)).pipe(control(0));
    constant(g, "a", 0, 0.25);
    auto& c1 = g.node("c1", "lfo");
    c1.params = {{"depth", 0.0}};
    c1.outputs["out"] = {control(0)};
    auto& c2 = g.node("c2", "lfo");
    c2.params = {{"depth", 0.0}};
    c2.outputs["out"] = {control(0)};
    const auto p = r.engine.build(g.spec, 1);
    CHECK(p->privatePipes.empty());
    CHECK(p->nodes.size() == 3);
    CHECK(listed(*p, 2) == std::vector<std::uint32_t>{1});  // control writers keep their order
}

TEST_CASE("writers of a feedback pipe write private pipes; its reader sees their sum a block later") {
    renderer r(1, 48000.0, 64);
    graph g;
    g.pipe(audio(0), 1, true).pipe(audio(1));
    constant(g, "a", 0, 0.1);
    constant(g, "b", 0, 0.2);
    auto& pass = g.node("pass", "gain");
    pass.params = {{"amount", 1.0}};
    pass.inputs["in"] = {audio(0)};
    pass.outputs["out"] = {audio(1)};
    auto& out = g.node("out", "audioOut");
    out.inputs["in"] = {audio(1)};
    const auto p = r.engine.build(g.spec, 1);
    REQUIRE(p->nodes.size() == 5);
    CHECK(p->nodes[2]->type() == "sum");
    CHECK(listed(*p, 1).empty());
    r.engine.play(p, 0.0, primitives::quantization::none);
    r.run(64);
    CHECK(r.out().back() == 0.0f);  // the first block reads the block before it
    r.run(64);
    CHECK(r.out().back() == 0.1f + 0.2f);
}

TEST_CASE("in a crossfade, each plan's summing node carries its writers: a removed writer fades out, kept "
          "ones hold") {
    renderer r(1, 48000.0, 64);
    r.play(writers({{"a", 0.1}, {"b", 0.2}, {"c", 0.3}}), 1);
    r.run(128);
    // c leaves and d arrives over 0.1 s (4800 samples); a and b stay.
    r.play(writers({{"a", 0.1}, {"b", 0.2}, {"d", 0.05}}), 2, 0.1);
    r.run(2400);
    const auto& out = r.out();
    // Halfway: a and b in full, c and d at about half.
    CHECK(out.back() == Approx(0.1 + 0.2 + 0.5 * 0.3 + 0.5 * 0.05).margin(0.002));
    r.run(4800);
    CHECK(r.out().back() == Approx(0.35).margin(1e-6));
}

TEST_CASE("a crossfade between plans of many writers renders the same on 1 and 4 threads") {
    auto render = [](int threads) {
        renderer r({.audio = {.sampleRate = 48000.0, .blockSize = 64}, .pipes = {}, .threads = threads}, 1);
        r.play(writers({{"a", 0.1}, {"b", 0.2}, {"c", 0.3}, {"e", 0.01}}), 1);
        r.run(256);
        r.play(writers({{"a", 0.1}, {"b", 0.2}, {"d", 0.05}, {"f", 0.02}}), 2, 0.01);
        r.run(1024);
        return r.out();
    };
    CHECK(render(4) == render(1));
}
