// Private pipes for writers of one audio pipe, and the summing nodes that
// add them (threads.md §4.2).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/plan.h>

#include "support/bits.h"
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

constexpr double coarse = primitives::defaultMinimumTaskCost;

// An engine of @p threads threads at a least task cost of @p minimum, in
// the order @p shuffle draws when given, with 64-sample blocks.
primitives::engineOptions options(int threads, double minimum = coarse,
                                  std::optional<std::uint64_t> shuffle = std::nullopt) {
    return {.audio = {.sampleRate = 48000.0, .blockSize = 64},
            .pipes = {},
            .threads = threads,
            .schedule = {.minimumTaskCost = minimum, .shuffleSeed = shuffle}};
}

// A sine of 0.1 at @p rate on audio pipe @p pipeIndex: unlike a constant,
// its block changes from block to block.
void wave(graph& g, const std::string& id, int pipeIndex, double rate) {
    auto& n = g.node(id, "lfo");
    n.params = {{"rate", rate}, {"depth", 0.1}};
    n.outputs["out"] = {audio(pipeIndex)};
}

// Sines at @p rates into audio pipe 0, which device channel 0 plays.
metadata::graphSpec waves(const std::vector<std::pair<std::string, double>>& rates) {
    graph g;
    g.pipe(audio(0));
    for (const auto& [id, rate] : rates) wave(g, id, 0, rate);
    g.node("out", "audioOut").inputs["in"] = {audio(0)};
    return g.spec;
}

// How many of @p p's private pipes forward (threads.md §4.2.5).
std::size_t forwarding(const primitives::plan& p) {
    return static_cast<std::size_t>(
        std::ranges::count_if(p.privatePipes, [](const auto& q) { return q->forwarding(); }));
}

// Channel 0 of each of @p p's private pipes.
std::vector<std::vector<float>> contents(const primitives::plan& p) {
    std::vector<std::vector<float>> out;
    for (const auto& q : p.privatePipes) {
        const auto c = q->read(0);
        out.emplace_back(c.begin(), c.end());
    }
    return out;
}

// @p g played on an engine of @p o for @p blocks blocks of 64 samples,
// with the plan's count of forwarding private pipes after the first block
// and after the last.
struct played {
    std::vector<float> out;
    std::size_t forwardingFirst = 0;
    std::size_t forwardingLast = 0;
    std::size_t privates = 0;
    bool parallel = false;
};

played render(const metadata::graphSpec& g, primitives::engineOptions o, int blocks) {
    renderer r(std::move(o), 1);
    const auto p = r.engine.build(g, 1);
    r.engine.play(p, 0.0, primitives::quantization::none);
    r.run(64);
    const std::size_t first = forwarding(*p);
    r.run(64 * (blocks - 1));
    return {.out = r.out(),
            .forwardingFirst = first,
            .forwardingLast = forwarding(*p),
            .privates = p->privatePipes.size(),
            .parallel = p->schedule.tasks.parallel};
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

TEST_CASE(
    "a crossfade between plans of many writers renders the same bits on 4 threads and in shuffled orders") {
    // On @p threads threads, at a least task cost of @p minimum, in the
    // order @p shuffle draws when given.
    auto render = [](int threads, double minimum, std::optional<std::uint64_t> shuffle) {
        renderer r({.audio = {.sampleRate = 48000.0, .blockSize = 64},
                    .pipes = {},
                    .threads = threads,
                    .schedule = {.minimumTaskCost = minimum, .shuffleSeed = shuffle}},
                   1);
        r.play(writers({{"a", 0.1}, {"b", 0.2}, {"c", 0.3}, {"e", 0.01}}), 1);
        r.run(256);
        r.play(writers({{"a", 0.1}, {"b", 0.2}, {"d", 0.05}, {"f", 0.02}}), 2, 0.01);
        r.run(1024);
        return r.out();
    };
    const auto one = render(1, coarse, std::nullopt);
    CHECK(support::bitDifference(render(4, coarse, std::nullopt), one) == "");
    // Each writer a task of its own, so the fade's writers run at once.
    CHECK(support::bitDifference(render(4, 0.0, std::nullopt), one) == "");
    const std::uint64_t seed = Catch::getSeed();  // --rng-seed repeats the orders
    INFO("shuffle seeds " << seed << " and " << seed + 1);
    CHECK(support::bitDifference(render(1, coarse, seed), one) == "");
    CHECK(support::bitDifference(render(1, coarse, seed + 1), one) == "");
}

TEST_CASE("on one thread the writers of one pipe add into it themselves and render the bits of 4 threads") {
    const auto g = waves({{"a", 50.0}, {"b", 70.0}, {"c", 90.0}, {"d", 110.0}, {"e", 130.0}});
    renderer one(options(1), 1);
    const auto p = one.engine.build(g, 1);
    REQUIRE(p->privatePipes.size() == 4);
    CHECK(forwarding(*p) == 0);
    one.engine.play(p, 0.0, primitives::quantization::none);
    // The swap's one block of crossfade writes the private pipes; the plan
    // alone forwards from the next.
    one.run(64);
    CHECK(forwarding(*p) == 4);
    const auto held = contents(*p);
    for (const auto& c : held) CHECK(support::peak(c) > 0.01);
    one.run(64 * 20);
    // Neither started nor written since: had the summing node still added
    // what they hold, the render would differ from the one below.
    CHECK(support::bitDifference(contents(*p), held) == "");

    // Each writer a task of its own: on 4 threads the writers run at once
    // into their private pipes, and the summing node adds them.
    const auto four = render(g, options(4, 0.0), 21);
    REQUIRE(four.parallel);
    CHECK(four.forwardingLast == 0);
    CHECK(support::bitDifference(one.out(), four.out) == "");
    // A schedule that is not parallel runs on the audio thread alone, in
    // plan order, so it forwards on 4 threads too.
    const auto chain = render(g, options(4), 21);
    REQUIRE_FALSE(chain.parallel);
    CHECK(chain.forwardingLast == 4);
    CHECK(support::bitDifference(chain.out, one.out()) == "");
}

TEST_CASE("a crossfade stops both plans forwarding and the plan left alone forwards again in the bits of 4 "
          "threads") {
    const auto first = waves({{"a", 50.0}, {"b", 70.0}, {"c", 90.0}, {"e", 110.0}});
    const auto second = waves({{"a", 50.0}, {"b", 70.0}, {"d", 130.0}, {"f", 150.0}});
    // A fade of 0.01 s is 480 samples, which ends in the eighth block.
    // Each plan alone forwards all of its private pipes when @p forwards
    // is true, and none otherwise.
    auto swap = [&](int threads, double minimum, bool forwards) {
        renderer r(options(threads, minimum), 1);
        const auto p1 = r.engine.build(first, 1);
        const auto p2 = r.engine.build(second, 2);
        REQUIRE(p1->privatePipes.size() == 3);
        REQUIRE(p2->privatePipes.size() == 3);
        const std::size_t all = forwards ? 3 : 0;
        r.engine.play(p1, 0.0, primitives::quantization::none);
        r.run(64 * 4);
        CHECK(forwarding(*p1) == all);
        r.engine.play(p2, 0.01, primitives::quantization::none);
        r.run(64);
        CHECK(forwarding(*p1) == 0);
        CHECK(forwarding(*p2) == 0);
        r.run(64 * 6);
        CHECK(forwarding(*p1) == 0);
        CHECK(forwarding(*p2) == 0);
        r.run(64);  // the fade's last block
        CHECK(forwarding(*p1) == 0);
        CHECK(forwarding(*p2) == all);
        r.run(64 * 4);
        return r.out();
    };
    const auto one = swap(1, coarse, true);
    // On 4 threads with each writer a task of its own, nothing forwards.
    CHECK(support::bitDifference(swap(4, 0.0, false), one) == "");
}

TEST_CASE(
    "a group whose writer adds to the pipe through two outputs does not forward and keeps the bits of 4 "
    "threads") {
    // An svf names pipe 0 at its output and its low tap, so it adds to its
    // private pipe twice a block: the summing node adds out + low, rounded
    // once, where adding each into the shared pipe would round twice.
    auto filtered = [](bool first) {
        graph g;
        g.pipe(audio(0)).pipe(audio(1)).pipe(audio(2));
        g.node("noise", "noise").outputs["out"] = {audio(1)};
        auto& quiet = g.node("quiet", "gain");
        quiet.params = {{"amount", 0.1}};
        quiet.inputs["in"] = {audio(1)};
        quiet.outputs["out"] = {audio(2)};
        auto svf = [&] {
            auto& f = g.node("f", "svf");
            f.params = {{"mode", std::string("band")}};
            f.inputs["in"] = {audio(2)};
            f.outputs["out"] = {audio(0)};
            f.outputs["low"] = {audio(0)};
        };
        if (first) svf();
        wave(g, "a", 0, 50.0);
        if (!first) svf();
        wave(g, "c", 0, 90.0);
        g.node("out", "audioOut").inputs["in"] = {audio(0)};
        return g.spec;
    };
    const auto later = filtered(false);
    const auto one = render(later, options(1), 20);
    REQUIRE(one.privates == 2);
    CHECK(one.forwardingLast == 0);
    const auto four = render(later, options(4, 0.0), 20);
    REQUIRE(four.parallel);
    CHECK(support::bitDifference(one.out, four.out) == "");

    // As the first writer, it adds to the shared pipe itself either way, so
    // the writers after it forward.
    const auto earlier = filtered(true);
    const auto lead = render(earlier, options(1), 20);
    CHECK(lead.forwardingLast == 2);
    CHECK(support::bitDifference(lead.out, render(earlier, options(4, 0.0), 20).out) == "");
}

TEST_CASE("a group with a reader between its writers does not forward and keeps the bits of 4 threads") {
    // Not a valid plan (ir::validate puts readers after writers), but one
    // the builder accepts: the reader takes the first writer's block only.
    graph g;
    g.pipe(audio(0)).pipe(audio(1));
    constant(g, "a", 0, 0.1);
    constant(g, "b", 0, 0.2);
    auto& reader = g.node("reader", "gain");
    reader.params = {{"amount", 1.0}};
    reader.inputs["in"] = {audio(0)};
    reader.outputs["out"] = {audio(1)};
    constant(g, "c", 0, 0.3);
    g.node("out", "audioOut").inputs["in"] = {audio(1)};
    const auto one = render(g.spec, options(1), 8);
    CHECK(one.forwardingLast == 0);
    CHECK(one.out.back() == 0.1f);
    CHECK(support::bitDifference(one.out, render(g.spec, options(4, 0.0), 8).out) == "");
}

TEST_CASE("with a shuffle seed no private pipe forwards") {
    const auto g = waves({{"a", 50.0}, {"b", 70.0}, {"c", 90.0}, {"d", 110.0}});
    const std::uint64_t seed = Catch::getSeed();  // --rng-seed repeats the order
    INFO("shuffle seed " << seed);
    const auto shuffled = render(g, options(1, coarse, seed), 20);
    REQUIRE(shuffled.privates == 3);
    CHECK(shuffled.forwardingFirst == 0);
    CHECK(shuffled.forwardingLast == 0);
    // The plan order forwards, in the same bits.
    const auto plain = render(g, options(1), 20);
    CHECK(plain.forwardingLast == 3);
    CHECK(support::bitDifference(shuffled.out, plain.out) == "");
}
