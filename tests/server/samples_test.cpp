// Audio files: decoding each format, resampling, the decoded-file cache,
// and buffers loaded through the server's loader (samples.md §4, §15.1).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/engine.h>
#include <espresso/protocol/messages.h>

#include "engineHost.h"
#include "samples.h"
#include "support/files.h"
#include "support/tempDir.h"
#include "support/unwrap.h"

using Catch::Matchers::ContainsSubstring;
using server::decodeAudioFile;

namespace {

std::filesystem::path fixture(const std::string& name) {
    return std::filesystem::path(ESPRESSO_SOURCE_DIR) / "tests/data/samples" / name;
}

// The samples every fixture holds, in eighths (make-fixtures.py).
std::vector<float> eighths(std::initializer_list<float> x) {
    std::vector<float> out;
    out.reserve(x.size());
    for (const float v : x) out.push_back(v / 8.0f);
    return out;
}
std::vector<float> left() { return eighths({0, 1, 2, 3, -1, -2, -3, -8}); }
std::vector<float> right() { return eighths({4, -4, 2, -2, 0, 7, -7, 1}); }

std::vector<double> sine(double freq, double rate, std::size_t n, double amplitude = 0.5) {
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = amplitude * std::sin(2.0 * std::numbers::pi * freq * static_cast<double>(i) / rate);
    }
    return x;
}

std::vector<float> toFloat(const std::vector<double>& x) { return {x.begin(), x.end()}; }

// @p in, one channel, resampled from @p from to @p to hertz.
std::vector<float> resampled(std::vector<float> in, double from, double to) {
    std::vector<std::vector<float>> one{std::move(in)};
    server::resample(one, from, to);
    return std::move(one.front());
}

// The amplitude of the component of @p x at @p freq, and the RMS of what
// remains, over the samples from @p from to @p to: a least-squares fit of
// a sine and a cosine at that frequency.
struct fit {
    double amplitude = 0.0;
    double residual = 0.0;
};

fit fitSine(std::span<const float> x, double freq, double rate, std::size_t from, std::size_t to) {
    double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
    for (std::size_t i = from; i < to; ++i) {
        const double w = 2.0 * std::numbers::pi * freq * static_cast<double>(i) / rate;
        const double s = std::sin(w);
        const double c = std::cos(w);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        xs += x[i] * s;
        xc += x[i] * c;
    }
    const double det = (ss * cc) - (sc * sc);
    const double a = ((xs * cc) - (xc * sc)) / det;
    const double b = ((xc * ss) - (xs * sc)) / det;
    double rest = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        const double w = 2.0 * std::numbers::pi * freq * static_cast<double>(i) / rate;
        const double e = x[i] - ((a * std::sin(w)) + (b * std::cos(w)));
        rest += e * e;
    }
    return {.amplitude = std::hypot(a, b), .residual = std::sqrt(rest / static_cast<double>(to - from))};
}

double decibels(double ratio) { return 20.0 * std::log10(ratio); }

}  // namespace

TEST_CASE("every format and bit depth decodes to the samples it holds (samples.md 4.1)") {
    for (const char* name :
         {"u8.wav", "s16.wav", "s24.wav", "s32.wav", "f32.wav", "f64.wav", "s8.aiff", "s16.aiff", "s24.aiff",
          "s32.aiff", "f32.aifc", "f64.aifc", "s16.flac", "s24.flac"}) {
        INFO(name);
        std::string error;
        const auto decoded = decodeAudioFile(fixture(name), error);
        INFO(error);
        const auto& d = support::unwrap(decoded);
        CHECK(d.sampleRate == 8000.0);
        REQUIRE(d.channels.size() == 2);
        CHECK(d.channels[0] == left());
        CHECK(d.channels[1] == right());
    }
}

TEST_CASE("MP3 and Ogg Vorbis decode to the tones they hold (samples.md 4.1)") {
    // Half a second at 22050 Hz: 440 Hz left, 660 Hz right, amplitude 0.5
    // (make-fixtures.py --lossy). Lossy, so the test fits each tone.
    for (const char* name : {"tone.mp3", "tone.ogg"}) {
        INFO(name);
        std::string error;
        const auto decoded = decodeAudioFile(fixture(name), error);
        INFO(error);
        const auto& d = support::unwrap(decoded);
        CHECK(d.sampleRate == 22050.0);
        REQUIRE(d.channels.size() == 2);
        // The length, with up to two frames of encoder padding.
        CHECK(d.frames() >= 11025);
        CHECK(d.frames() <= 11025 + 2 * 1152);
        for (const auto& [channel, freq] : {std::pair{0, 440.0}, std::pair{1, 660.0}}) {
            const auto f = fitSine(d.channels[static_cast<std::size_t>(channel)], freq, 22050.0, 3000, 9000);
            // Measured: LAME at 96 kbit/s keeps the level within 0.5 dB and
            // the rest 78 dB down; libvorbis at quality 4, 0.02 dB and 37 dB.
            CHECK(std::abs(decibels(f.amplitude / 0.5)) < 1.0);
            CHECK(decibels(f.residual / (0.5 / std::numbers::sqrt2)) < -30.0);
        }
    }
}

TEST_CASE("a file that is missing or not audio does not decode") {
    const support::tempDir dir;
    std::string error;
    CHECK_FALSE(decodeAudioFile(dir / "missing.wav", error));
    CHECK_THAT(error, ContainsSubstring("no such file"));

    dir.write("notes.txt", "a text file, not audio");
    CHECK_FALSE(decodeAudioFile(dir / "notes.txt", error));
    CHECK_THAT(error, ContainsSubstring("not an audio file in a known format"));

    // A WAV header cut off before its data.
    dir.write("cut.wav", support::readText(fixture("s16.wav")).substr(0, 20));
    CHECK_FALSE(decodeAudioFile(dir / "cut.wav", error));
    CHECK_THAT(error, ContainsSubstring("not a WAV or AIFF file"));
}

TEST_CASE("resampling keeps a sine's level and adds nothing 90 dB up (samples.md 4.3)") {
    // 44.1 kHz to 48 kHz and back, and down from 48 kHz, with tones low in
    // the band and at 0.45 of the lower rate. The ends, where the kernel
    // runs off the signal, are left out.
    struct conversion {
        double from;
        double to;
    };
    for (const auto [from, to] :
         {conversion{.from = 44100, .to = 48000}, conversion{.from = 48000, .to = 44100},
          conversion{.from = 48000, .to = 22050}}) {
        const double lower = std::min(from, to);
        for (const double freq : {997.0, 0.45 * lower}) {
            INFO(from << " Hz to " << to << " Hz, " << freq << " Hz");
            const auto in = toFloat(sine(freq, from, 9600));
            const auto out = resampled(in, from, to);
            CHECK(out.size() == static_cast<std::size_t>(std::llround(9600.0 * to / from)));
            const auto f = fitSine(out, freq, to, 400, out.size() - 400);
            CHECK(std::abs(decibels(f.amplitude / 0.5)) < 0.01);
            CHECK(decibels(f.residual / (0.5 / std::numbers::sqrt2)) < -90.0);
        }
    }
    // Round trip.
    const auto in = toFloat(sine(1000.0, 44100, 8820));
    const auto back = resampled(resampled(in, 44100, 48000), 48000, 44100);
    REQUIRE(back.size() == in.size());
    const auto f = fitSine(back, 1000.0, 44100, 400, back.size() - 400);
    CHECK(std::abs(decibels(f.amplitude / 0.5)) < 0.01);
    CHECK(decibels(f.residual / (0.5 / std::numbers::sqrt2)) < -90.0);
}

TEST_CASE("resampling down removes what the lower rate cannot hold") {
    // A tone at 26 kHz, above the 22.05 kHz limit of the output and past
    // the filter's transition, folds back at no more than -90 dB.
    const auto in = toFloat(sine(26000.0, 96000, 19200));
    const auto out = resampled(in, 96000, 44100);
    double sum = 0.0;
    for (std::size_t i = 400; i + 400 < out.size(); ++i) sum += static_cast<double>(out[i]) * out[i];
    const double rms = std::sqrt(sum / static_cast<double>(out.size() - 800));
    CHECK(decibels(rms / (0.5 / std::numbers::sqrt2)) < -90.0);
}

TEST_CASE("resampling at equal rates returns the input") {
    const std::vector<float> x{0.1f, -0.2f, 0.3f};
    CHECK(resampled(x, 48000, 48000) == x);
    CHECK(resampled({}, 44100, 48000).empty());
}

TEST_CASE("the cache decodes a file once per sample rate and again when it changes (samples.md 4.5)") {
    const support::tempDir dir;
    dir.write("a.wav", support::readText(fixture("s16.wav")));
    server::sampleCache cache;
    std::string error;
    const auto first = cache.load(dir / "a.wav", 8000, error);
    REQUIRE(first);
    CHECK(cache.load(dir / "a.wav", 8000, error) == first);
    CHECK(cache.load(dir / "sub/../a.wav", 8000, error) == first);  // the same file, resolved
    CHECK(cache.decodes() == 1);
    // At another rate, it is decoded and resampled again.
    const auto faster = cache.load(dir / "a.wav", 16000, error);
    REQUIRE(faster);
    CHECK(faster->frames() == 16);
    CHECK(cache.decodes() == 2);
    // At rate 0, it keeps its own rate and samples (samples.md 4.5.1).
    const auto own = cache.load(dir / "a.wav", 0, error);
    REQUIRE(own);
    CHECK(own->channels == support::unwrap(server::decodeAudioFile(dir / "a.wav", error)).channels);
    CHECK(cache.decodes() == 3);
    // An edited file is decoded again.
    dir.write("a.wav", support::readText(fixture("s24.wav")));
    std::filesystem::last_write_time(dir / "a.wav", std::filesystem::last_write_time(dir / "a.wav") +
                                                        std::chrono::seconds(5));
    CHECK(cache.load(dir / "a.wav", 8000, error) != first);
    CHECK(cache.decodes() == 4);
    CHECK_FALSE(cache.load(dir / "missing.wav", 8000, error));
    CHECK_THAT(error, ContainsSubstring("no such file"));
}

TEST_CASE("the cache keeps the most recently used files") {
    const support::tempDir dir;
    server::sampleCache cache;
    std::string error;
    const auto name = [](std::size_t i) { return "f" + std::to_string(i) + ".wav"; };
    for (std::size_t i = 0; i <= server::sampleCache::capacity; ++i)
        dir.write(name(i), support::readText(fixture("s16.wav")));
    for (std::size_t i = 0; i <= server::sampleCache::capacity; ++i)
        REQUIRE(cache.load(dir / name(i), 8000, error));
    const std::size_t decoded = cache.decodes();
    REQUIRE(cache.load(dir / name(server::sampleCache::capacity), 8000, error));  // the newest stays
    CHECK(cache.decodes() == decoded);
    REQUIRE(cache.load(dir / name(0), 8000, error));  // the oldest went
    CHECK(cache.decodes() == decoded + 1);
}

TEST_CASE("a piece's buffer loads a file under the project root at the engine's rate (samples.md 4.2)") {
    const support::tempDir dir;
    dir.write("samples/hit.wav", support::readText(fixture("s16.wav")));
    primitives::engine audio(
        {.audio = {.sampleRate = 16000.0, .blockSize = 64},
         .pipes = {},
         .loadSample = server::sampleLoaderFor(dir.path(), std::make_shared<server::sampleCache>()),
         .bufferMemory = primitives::bufferPool::defaultLimit});
    metadata::graphSpec g;
    g.buffers = {{.buffer = {.index = 0},
                  .id = "main.lisp/main/buffer@3:9#0",
                  .channels = 2,
                  .seconds = std::nullopt,
                  .file = "samples/hit.wav",
                  .src = std::nullopt}};
    protocol::planError error;
    const auto plan = server::buildFromIr(audio, ir::write(g), 1, error).plan;
    INFO(error.message);
    REQUIRE(plan);
    REQUIRE(plan->buffers.size() == 1);
    CHECK(plan->buffers[0]->frames() == 16);  // 8 frames at 8 kHz, at 16 kHz

    g.buffers[0].file = "samples/missing.wav";
    CHECK_FALSE(server::buildFromIr(audio, ir::write(g), 2, error).plan);
    CHECK(error.message ==
          "buffer main.lisp/main/buffer@3:9#0: cannot load samples/missing.wav: no such file, or it cannot "
          "be read");
    // The failure names the buffer, so a client can find its form.
    CHECK(error.subject == protocol::failSubject::buffer);
    CHECK(error.id == "main.lisp/main/buffer@3:9#0");
}
