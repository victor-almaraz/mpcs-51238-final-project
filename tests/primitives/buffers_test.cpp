// Buffers in the engine: shape, filling from files, adoption across swaps,
// and the memory bound (samples.md §3, §4.4, §5).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/plan.h>

#include "support/graph.h"

using Catch::Matchers::ContainsSubstring;
using metadata::bufferDecl;
using primitives::buildError;
using primitives::decodedAudio;
using primitives::sampleBuffer;

namespace {

constexpr double rate = 1000.0;  // one frame a millisecond, for short buffers

// Files the fake loader knows: a stereo ramp of 5 frames and a mono one of 3.
std::shared_ptr<const decodedAudio> fakeFiles(std::string_view path, double, std::string& error) {
    if (path == "stereo.wav")
        return std::make_shared<decodedAudio>(decodedAudio{{{1, 2, 3, 4, 5}, {-1, -2, -3, -4, -5}}});
    if (path == "mono.wav") return std::make_shared<decodedAudio>(decodedAudio{{{7, 8, 9}}});
    if (path == "empty.wav") return std::make_shared<decodedAudio>(decodedAudio{{{}}});
    error = "no such file";
    return nullptr;
}

primitives::engine makeEngine(std::size_t bufferMemory = primitives::bufferPool::defaultLimit) {
    return primitives::engine({.audio = {.sampleRate = rate, .blockSize = 16},
                               .pipes = {},
                               .loadImage = nullptr,
                               .loadSample = fakeFiles,
                               .bufferMemory = bufferMemory});
}

bufferDecl decl(std::string id, int channels, std::optional<double> seconds,
                std::optional<std::string> file = {}) {
    return {.buffer = {.index = 0},
            .id = std::move(id),
            .channels = channels,
            .seconds = seconds,
            .file = std::move(file),
            .src = std::nullopt};
}

// A graph that declares @p buffers and plays a sine.
metadata::graphSpec withBuffers(std::vector<bufferDecl> buffers) {
    support::graph g;
    g.pipe(support::audio(0));
    g.node("sine", "sineOsc").outputs["out"] = {support::audio(0)};
    for (std::size_t i = 0; i < buffers.size(); ++i) buffers[i].buffer.index = static_cast<std::int32_t>(i);
    g.spec.buffers = std::move(buffers);
    return g.spec;
}

std::vector<float> samples(const sampleBuffer& b, int channel) {
    const auto c = b.channel(channel);
    return {c.begin(), c.end()};
}

// The buildError that building @p g throws: what it is about, and its
// message after the ID it names.
struct refused {
    buildError::subject about = buildError::subject::node;
    std::string message;
};

refused refusal(primitives::engine& e, const metadata::graphSpec& g) {
    try {
        e.build(g, 1);
    } catch (const buildError& err) {
        return {.about = err.about(), .message = (err.node().empty() ? "" : err.node() + ": ") + err.what()};
    }
    FAIL("the plan was built");
    return {};
}

}  // namespace

TEST_CASE("a buffer is planar silent and as long as declared") {
    sampleBuffer b(2, 4);
    CHECK(b.channels() == 2);
    CHECK(b.frames() == 4);
    CHECK(b.bytes() == std::size_t{2} * 4 * sizeof(float));
    b.channel(1)[3] = 0.5f;
    CHECK(samples(b, 0) == std::vector<float>{0, 0, 0, 0});
    CHECK(samples(b, 1) == std::vector<float>{0, 0, 0, 0.5f});
}

TEST_CASE("filling maps a file's channels to the buffer's (samples.md 4.4)") {
    const decodedAudio stereo{{{1, 2}, {3, 5}}};
    SECTION("equal counts fill channel by channel") {
        sampleBuffer b(2, 2);
        b.fill(stereo);
        CHECK(samples(b, 0) == std::vector<float>{1, 2});
        CHECK(samples(b, 1) == std::vector<float>{3, 5});
    }
    SECTION("a mono file fills every channel") {
        sampleBuffer b(3, 2);
        b.fill(decodedAudio{{{4, 6}}});
        for (int c = 0; c < 3; ++c) CHECK(samples(b, c) == std::vector<float>{4, 6});
    }
    SECTION("a mono buffer takes the average") {
        sampleBuffer b(1, 2);
        b.fill(stereo);
        CHECK(samples(b, 0) == std::vector<float>{2, 3.5f});
    }
    SECTION("otherwise the shared channels fill and the rest stay silent") {
        sampleBuffer b(3, 2);
        b.fill(stereo);
        CHECK(samples(b, 1) == std::vector<float>{3, 5});
        CHECK(samples(b, 2) == std::vector<float>{0, 0});
        sampleBuffer fewer(2, 2);
        fewer.fill(decodedAudio{{{1, 1}, {2, 2}, {3, 3}}});
        CHECK(samples(fewer, 1) == std::vector<float>{2, 2});
    }
    SECTION("a longer file is cut and a shorter one leaves the rest silent") {
        sampleBuffer shorter(2, 1);
        shorter.fill(stereo);
        CHECK(samples(shorter, 1) == std::vector<float>{3});
        sampleBuffer longer(2, 4);
        longer.fill(stereo);
        CHECK(samples(longer, 0) == std::vector<float>{1, 2, 0, 0});
    }
}

TEST_CASE("a plan makes its buffers from their lengths and files (samples.md 5.1)") {
    auto e = makeEngine();
    const auto p =
        e.build(withBuffers({decl("silent", 2, 0.004), decl("file", 2, std::nullopt, "stereo.wav"),
                             decl("cut", 1, 0.002, "mono.wav"), decl("padded", 1, 0.006, "mono.wav")}),
                1);
    REQUIRE(p->buffers.size() == 4);
    CHECK(p->buffers[0]->frames() == 4);
    CHECK(samples(*p->buffers[0], 1) == std::vector<float>{0, 0, 0, 0});
    // Without a length, the buffer takes its file's.
    CHECK(p->buffers[1]->frames() == 5);
    CHECK(samples(*p->buffers[1], 1) == std::vector<float>{-1, -2, -3, -4, -5});
    CHECK(samples(*p->buffers[2], 0) == std::vector<float>{7, 8});
    CHECK(samples(*p->buffers[3], 0) == std::vector<float>{7, 8, 9, 0, 0, 0});
}

TEST_CASE("an incoming buffer of the same ID shape and file keeps its contents (samples.md 5.4)") {
    auto e = makeEngine();
    const auto first = e.build(withBuffers({decl("ring", 2, 0.004, "stereo.wav")}), 1);
    first->buffers[0]->channel(0)[0] = 42.0f;  // as a writer would have recorded

    const auto same = e.build(withBuffers({decl("ring", 2, 0.004, "stereo.wav")}), 2);
    CHECK(same->buffers[0] == first->buffers[0]);
    CHECK(same->buffers[0]->channel(0)[0] == 42.0f);

    // A change of ID, channels, length, or file makes a new buffer.
    for (const auto& changed :
         {decl("other", 2, 0.004, "stereo.wav"), decl("ring", 1, 0.004, "stereo.wav"),
          decl("ring", 2, 0.003, "stereo.wav"), decl("ring", 2, 0.004, "mono.wav"), decl("ring", 2, 0.004)}) {
        const auto p = e.build(withBuffers({changed}), 3);
        CHECK(p->buffers[0] != first->buffers[0]);
        CHECK(p->buffers[0]->channel(0)[0] != 42.0f);
    }
}

TEST_CASE("a buffer lives as long as a plan holds it") {
    auto e = makeEngine();
    std::weak_ptr<sampleBuffer> weak;
    {
        const auto p = e.build(withBuffers({decl("ring", 1, 0.004)}), 1);
        weak = p->buffers[0];
        p->buffers[0]->channel(0)[0] = 1.0f;
    }
    CHECK(weak.expired());
    // With no plan holding it, the same declaration makes a silent buffer.
    const auto again = e.build(withBuffers({decl("ring", 1, 0.004)}), 2);
    CHECK(again->buffers[0]->channel(0)[0] == 0.0f);
}

TEST_CASE("buffers are bounded by the engine's buffer memory (samples.md 5.6)") {
    // 100 frames of one channel is 400 bytes; the bound is 1000.
    auto e = makeEngine(1000);
    const auto held = e.build(withBuffers({decl("a", 1, 0.1)}), 1);
    // Adopting a live buffer costs nothing more.
    CHECK_NOTHROW(e.build(withBuffers({decl("a", 1, 0.1), decl("b", 1, 0.1)}), 2));
    const auto message = refusal(e, withBuffers({decl("a", 1, 0.1), decl("c", 2, 0.1)})).message;
    CHECK_THAT(message,
               ContainsSubstring(
                   "the plan's buffers, frames, and spectral history need 1 MB with those already playing"));
    CHECK_THAT(message, ContainsSubstring("(--buffer-memory)"));
    // A length far past the bound is refused, not allocated.
    CHECK_THAT(refusal(e, withBuffers({decl("huge", 8, 1e300)})).message,
               ContainsSubstring("need more than 1000000000000 MB"));
}

TEST_CASE("a buffer whose file does not load refuses the plan at the buffer (samples.md 4.6)") {
    auto e = makeEngine();
    const auto missing = refusal(e, withBuffers({decl("ring", 1, 1.0, "missing.wav")}));
    CHECK(missing.message == "ring: cannot load missing.wav: no such file");
    CHECK(missing.about == buildError::subject::buffer);
    CHECK(refusal(e, withBuffers({decl("ring", 1, std::nullopt, "empty.wav")})).message ==
          "ring: empty.wav has no audio");

    primitives::engine noLoader({.audio = {.sampleRate = rate, .blockSize = 16}, .pipes = {}});
    CHECK(refusal(noLoader, withBuffers({decl("ring", 1, 1.0, "mono.wav")})).message ==
          "ring: this engine cannot load audio files");
}
