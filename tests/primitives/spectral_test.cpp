// fft and ifft: the round trip of samples.md §10.2 at every size and
// overlap, frames at hops shorter and longer than a block, one frame pipe
// read by two nodes, and frames a reader cannot use.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>

#include "support/graph.h"

using Catch::Approx;
using support::audio;
using support::events;

namespace {

// White noise from a fixed linear congruential generator, at 0.5.
std::vector<float> noise(std::size_t n) {
    std::vector<float> x(n);
    std::uint32_t s = 12345;
    for (auto& v : x) {
        s = (s * 1664525u) + 1013904223u;
        v = (static_cast<float>(s >> 8u) / 16777216.0f - 0.5f);
    }
    return x;
}

// Device input -> fft -> ifft -> output 0, and a second ifft of the same
// frames to output 1.
support::graph roundTrip(double size, double overlap) {
    support::graph g;
    g.pipe(audio(0)).pipe(events(1)).pipe(audio(2)).pipe(audio(3));
    g.node("in", "audioIn").outputs["out"] = {audio(0)};
    auto& f = g.node("fft", "fft");
    f.params["size"] = size;
    f.params["overlap"] = overlap;
    f.inputs["in"] = {audio(0)};
    f.outputs["out"] = {events(1)};
    for (const auto& [id, pipe] : {std::pair{"ifft", audio(2)}, std::pair{"ifft2", audio(3)}}) {
        auto& i = g.node(id, "ifft");
        i.inputs["in"] = {events(1)};
        i.outputs["out"] = {pipe};
    }
    g.node("out", "audioOut").inputs["in"] = {audio(2)};
    auto& second = g.node("out2", "audioOut");
    second.params["channel"] = 1.0;
    second.inputs["in"] = {audio(3)};
    return g;
}

}  // namespace

TEST_CASE("fft then ifft reproduces the input exactly size samples later (samples.md 10.2)") {
    for (const double size : {256.0, 512.0, 1024.0, 2048.0, 4096.0, 8192.0, 16384.0}) {
        for (const double overlap : {2.0, 4.0}) {
            // Blocks shorter and longer than the hop: one frame in some
            // blocks and none in others, or several in each.
            for (const int block : {64, 1024}) {
                INFO("size " << size << ", overlap " << overlap << ", block " << block);
                const auto n = static_cast<std::size_t>(size);
                support::renderer r(2, 48000.0, block);
                r.play(roundTrip(size, overlap).spec);
                const auto x = noise(3 * n);
                r.run(static_cast<int>(3 * n), {x});
                double worst = 0.0;
                double worstSecond = 0.0;
                for (std::size_t t = 0; t < 3 * n; ++t) {
                    const double expected = t < n ? 0.0 : x[t - n];
                    worst = std::max(worst, std::abs(r.out(0)[t] - expected));
                    worstSecond = std::max(worstSecond, std::abs(r.out(1)[t] - expected));
                }
                CHECK(worst < 1e-5);
                CHECK(worstSecond < 1e-5);  // a second reader of the same frames
            }
        }
    }
}

TEST_CASE("an fft analyzes every channel of its input") {
    support::graph g;
    g.pipe(audio(0)).pipe(audio(1), 2).pipe(events(2)).pipe(audio(3), 2);
    g.node("in", "audioIn").outputs["out"] = {audio(0)};
    // Panned hard left: channel 0 is the input, channel 1 silent.
    auto& left = g.node("pan", "pan");
    left.params["position"] = -1.0;
    left.inputs["in"] = {audio(0)};
    left.outputs["out"] = {audio(1)};
    auto& f = g.node("fft", "fft");
    f.params["size"] = 256.0;
    f.inputs["in"] = {audio(1)};
    f.outputs["out"] = {events(2)};
    auto& i = g.node("ifft", "ifft");
    i.inputs["in"] = {events(2)};
    i.outputs["out"] = {audio(3)};
    g.node("out", "audioOut").inputs["in"] = {audio(3)};
    support::renderer r(2, 48000.0, 64);
    r.play(g.spec);
    const auto x = noise(1024);
    r.run(1024, {x});
    for (std::size_t t = 256; t < 1024; ++t) {
        CHECK(r.out(0)[t] == Approx(x[t - 256]).margin(1e-5));
        CHECK(r.out(1)[t] == Approx(0.0).margin(1e-6));
    }
}
