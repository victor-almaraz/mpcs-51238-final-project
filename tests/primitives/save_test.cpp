// bufferSave: snapshots copied a bounded number of frames a block and
// handed to the control thread through a save port (samples.md §18).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/io.h>

#include "support/graph.h"

namespace {

constexpr double rate = 1000.0;
constexpr int block = 16;
constexpr std::size_t frames = 200;  // 0.2 s: four blocks of copying at 64 frames a block

// A 200-frame buffer saved to take.wav by a metro every @p every beats at
// 937.5 BPM (a beat is 64 samples, four blocks).
metadata::graphSpec saving(double every) {
    support::graph g;
    g.spec.tempo = 937.5;
    g.spec.buffers = {{.buffer = {.index = 0},
                       .id = "take",
                       .channels = 1,
                       .seconds = static_cast<double>(frames) / rate,
                       .file = std::nullopt,
                       .src = std::nullopt}};
    g.pipe(support::events(0));
    auto& clock = g.node("clock", "metro");
    clock.params["every"] = every;
    clock.outputs["out"] = {support::events(0)};
    auto& save = g.node("save", "bufferSave");
    save.params["buffer"] = metadata::bufferRef{.index = 0};
    save.params["file"] = std::string("takes/take.wav");
    save.inputs["trigger"] = {support::events(0)};
    return g.spec;
}

// The snapshots that are complete, as drainSaves passes them.
struct saved {
    std::string node;
    std::string bufferId;
    std::string file;
    double sampleRate = 0.0;
    std::vector<float> samples;
};

std::vector<saved> drain(primitives::engine& e) {
    std::vector<saved> out;
    e.io().drainSaves([&](const primitives::savePort& p, const primitives::sampleBuffer& snapshot) {
        const auto first = snapshot.channel(0);
        out.push_back({.node = p.owner(),
                       .bufferId = p.bufferId(),
                       .file = p.file(),
                       .sampleRate = p.sampleRate(),
                       .samples = {first.begin(), first.end()}});
    });
    return out;
}

}  // namespace

TEST_CASE("bufferSave copies its buffer over blocks and hands over the whole snapshot (samples.md 18.3)") {
    support::renderer r(1, rate, block);
    const auto p = r.engine.build(saving(1000.0), 1);  // one trigger, at the start
    auto& buffer = *p->buffers[0];
    for (std::size_t i = 0; i < frames; ++i) buffer.channel(0)[i] = static_cast<float>(i);
    r.engine.play(p, 0.0, primitives::quantization::none);

    // 64 frames a block: blocks 0 to 3 copy, and the last completes it.
    for (int b = 0; b < 3; ++b) {
        r.run(block);
        CHECK(drain(r.engine).empty());
    }
    r.run(block);
    buffer.channel(0)[0] = -1.0f;  // after the copy: not in the snapshot
    const auto s = drain(r.engine);
    REQUIRE(s.size() == 1);
    CHECK(s[0].node == "save");
    CHECK(s[0].bufferId == "take");
    CHECK(s[0].file == "takes/take.wav");
    CHECK(s[0].sampleRate == rate);
    REQUIRE(s[0].samples.size() == frames);
    for (std::size_t i = 0; i < frames; ++i) CHECK(s[0].samples[i] == static_cast<float>(i));
    // Written once.
    r.run(block);
    CHECK(drain(r.engine).empty());
}

TEST_CASE("a trigger while a save is pending is dropped and counted (samples.md 18.3)") {
    support::renderer r(1, rate, block);
    r.play(saving(1.0), 1);  // a trigger every four blocks
    // The first save is complete after four blocks and is never written,
    // so the triggers at 64 and 128 samples find it pending.
    r.run(block * 9);
    CHECK(r.engine.io().drops() == 2);
    CHECK(drain(r.engine).size() == 1);
    // Written, the port takes the next trigger, at 192 samples.
    r.run(block * 4);
    CHECK(r.engine.io().drops() == 2);
    r.run(block * 4);
    CHECK(drain(r.engine).size() == 1);
}

TEST_CASE("a bufferSave's snapshot counts against buffer memory (samples.md 18.4)") {
    // Room for the buffer, not for it and its snapshot.
    primitives::engine e({.audio = {.sampleRate = rate, .blockSize = block},
                          .pipes = {},
                          .loadImage = nullptr,
                          .loadSample = nullptr,
                          .bufferMemory = frames * sizeof(float) * 3 / 2});
    CHECK_THROWS_WITH(e.build(saving(1.0), 1), Catch::Matchers::ContainsSubstring("--buffer-memory"));
}
