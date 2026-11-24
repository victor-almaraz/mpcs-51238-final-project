// Offline rendering and WAVE output.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <sys/resource.h>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/frame.h>

#include "offline.h"
#include "support/bits.h"
#include "support/files.h"
#include "support/graph.h"
#include "support/tempDir.h"

using namespace support;

namespace {

metadata::graphSpec noisy() {
    graph g;
    g.spec.seed = 5;
    g.pipe(audio(0));
    g.node("noise", "noise").outputs["out"] = {audio(0)};
    g.node("out", "audioOut").inputs["in"] = {audio(0)};
    return g.spec;
}

std::vector<std::vector<float>> renderNoise() {
    primitives::engine e({.audio = {.sampleRate = 48000.0, .blockSize = 100}, .pipes = {}});
    return server::renderOffline(e, e.build(noisy(), 1), 1050, 2);
}

} // namespace

TEST_CASE("offline renders repeat exactly and fill the requested frames") {
    const auto a = renderNoise();
    REQUIRE(a.size() == 2);
    CHECK(a[0].size() == 1050);  // a partial last block
    CHECK(support::bitDifference(renderNoise(), a) == "");
    CHECK(rms(a[0]) > 0.1);
    CHECK(peak(a[1]) == 0.0);  // mono into output 0 only
}

TEST_CASE("an offline render calls back after each block, a partial last one included (samples.md 18.4)") {
    primitives::engine e({.audio = {.sampleRate = 48000.0, .blockSize = 100}, .pipes = {}});
    int blocks = 0;
    server::renderOffline(e, e.build(noisy(), 1), 1050, 1, 0.0, {}, [&] { ++blocks; });
    CHECK(blocks == 11);
}

TEST_CASE("WAVE files hold 32-bit float samples") {
    tempDir dir;
    const auto path = (dir / "x.wav").string();
    REQUIRE(server::writeWav(path, std::vector<std::vector<float>>{{0.5f, -0.25f}, {1.0f, 0.0f}}, 44100.0));
    const std::string bytes = support::readText(path);
    // Header: RIFF (12) + fmt chunk (8 + 18) + fact chunk (8 + 4) + data header (8) = 58.
    REQUIRE(server::wavHeaderBytes == 58);
    REQUIRE(bytes.size() == 58 + 16);  // 2 frames x 2 channels x 4 bytes
    // The little-endian field of type T at byte @p at.
    const auto field = [&bytes]<typename T>(std::size_t at, T) {
        std::array<char, sizeof(T)> raw{};
        bytes.copy(raw.data(), raw.size(), at);
        return std::bit_cast<T>(raw);
    };
    CHECK(bytes.substr(0, 4) == "RIFF");
    CHECK(field(4, std::uint32_t{}) == bytes.size() - 8);
    CHECK(bytes.substr(8, 8) == "WAVEfmt ");
    CHECK(field(16, std::uint32_t{}) == 18);  // fmt chunk size, with cbSize
    CHECK(field(20, std::uint16_t{}) == 3);   // WAVE_FORMAT_IEEE_FLOAT
    CHECK(field(22, std::uint16_t{}) == 2);   // channels
    CHECK(field(24, std::uint32_t{}) == 44100);
    CHECK(field(28, std::uint32_t{}) == 44100 * 8);  // bytes per second
    CHECK(field(32, std::uint16_t{}) == 8);          // block align
    CHECK(field(34, std::uint16_t{}) == 32);         // bits per sample
    CHECK(field(36, std::uint16_t{}) == 0);          // cbSize
    CHECK(bytes.substr(38, 4) == "fact");
    CHECK(field(42, std::uint32_t{}) == 4);
    CHECK(field(46, std::uint32_t{}) == 2);  // frames
    CHECK(bytes.substr(50, 4) == "data");
    CHECK(field(54, std::uint32_t{}) == 16);
    const auto frame = field(58, std::array<float, 4>{});
    CHECK(frame[0] == 0.5f);  // interleaved: L0 R0 L1 R1
    CHECK(frame[1] == 1.0f);
    CHECK(frame[2] == -0.25f);

    // 32-bit RIFF size: header after the first 8 bytes plus samples.
    CHECK(server::maxWavFrames(2) == (std::size_t{0xFFFFFFFF} - 50) / 8);
    CHECK(server::maxWavFrames(2) > std::size_t{48000} * 60 * 60 * 3);
    CHECK(server::maxWavFrames(2) < std::size_t{48000} * 86400);
    CHECK_FALSE(server::readText((dir / "missing.ir.json").string()));
}

namespace {

// Limits the size of files this process writes, for the scope, and
// ignores SIGXFSZ, so a write past the limit fails with EFBIG instead of
// ending the process. setrlimit takes its limits and std::signal its
// disposition by pointer: the narrow unsafe use, confined to this guard.
class fileSizeLimit {
public:
    explicit fileSizeLimit(rlim_t bytes) {
        REQUIRE(getrlimit(RLIMIT_FSIZE, &previous_) == 0);
        REQUIRE(std::signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
        rlimit limit = previous_;
        limit.rlim_cur = bytes;
        REQUIRE(setrlimit(RLIMIT_FSIZE, &limit) == 0);  // without the limit the test proves nothing
    }
    ~fileSizeLimit() {
        CHECK(setrlimit(RLIMIT_FSIZE, &previous_) == 0);
        CHECK(std::signal(SIGXFSZ, SIG_DFL) != SIG_ERR);  // the test process's disposition
    }
    fileSizeLimit(const fileSizeLimit&) = delete;
    fileSizeLimit& operator=(const fileSizeLimit&) = delete;

private:
    rlimit previous_{};
};

}  // namespace

TEST_CASE("WAVE output reports failure when the samples do not reach the file") {
    tempDir dir;
    const auto path = (dir / "short.wav").string();
    const std::vector<std::vector<float>> channels(2, std::vector<float>(48000, 0.25f));  // 384 KB
    {
        const fileSizeLimit limit(rlim_t{64} * 1024);
        CHECK_FALSE(server::writeWav(path, channels, 48000.0));
    }
    CHECK(server::writeWav(path, channels, 48000.0));
}

TEST_CASE("an offline render passes each frame time's visual layer in order") {
    primitives::engine engine(
        {.audio = {.sampleRate = 48000.0, .blockSize = 256, .frameWidth = 4, .frameHeight = 4},
         .pipes = {.video = 1}});
    std::vector<std::size_t> indices;
    std::vector<std::uint32_t> colors;
    server::renderOffline(engine, engine.build(shown({1.0, 0.0, 0.0, 1.0}), 1), 24000, 1, 10.0,
                          [&](std::size_t index, const primitives::frame& f) {
                              indices.push_back(index);
                              colors.push_back(f.at(0, 0));
                          });
    CHECK(indices == std::vector<std::size_t>{0, 1, 2, 3, 4});  // half a second at 10 frames per second
    CHECK(std::ranges::all_of(colors, [](std::uint32_t c) { return c == 0xffff0000u; }));
}
