// The example pieces play as described: their IR files run on the engine
// with MIDI and audio input.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <proj/ir/ir.h>
#include <proj/metadata/graph.h>
#include <proj/primitives/engine.h>
#include <proj/primitives/frame.h>
#include <proj/primitives/io.h>
#include <proj/primitives/random.h>

#include "offline.h"
#include "support/graph.h"
#include "support/unwrap.h"

using Catch::Approx;
using support::peak;
using support::rms;

namespace {

constexpr int rate = 48000;

// The graph in examples/NAME.ir.json.
metadata::graphSpec exampleGraph(const std::string& name) {
    std::ifstream in(std::filesystem::path(PROJ_SOURCE_DIR) / "examples" / (name + ".ir.json"));
    std::stringstream text;
    text << in.rdbuf();
    const auto ir = ir::read(text.str());
    INFO(ir.text());
    REQUIRE(ir.ok());
    return support::unwrap(ir.graph);
}

// Plays examples/NAME.ir.json, feeding the same input to device inputs 0
// and 1.
class example {
public:
    explicit example(const std::string& name) : r_(2, rate, 128) {
        r_.play(exampleGraph(name));
        run(128);
    }

    void run(int frames, std::vector<float> input = {}) {
        input.resize(static_cast<std::size_t>(frames), 0.0f);
        r_.run(frames, {input, input});
    }

    void midi(std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
        primitives::midiPacket p;
        p.size = 3;
        p.bytes = {status, d1, d2};
        r_.engine.io().deliverMidi("Keyboard", p);
    }

    const std::vector<float>& out(int ch) const { return r_.out(ch); }
    std::size_t now() const { return r_.out(0).size(); }

private:
    support::renderer r_;
};

bool finite(const std::vector<float>& x) {
    return std::ranges::all_of(x, [](float v) { return std::isfinite(v); });
}

std::vector<float> impulse(int frames) {
    std::vector<float> x(static_cast<std::size_t>(frames), 0.0f);
    x[0] = 1.0f;
    return x;
}

// Channel values of a frame pixel (0xAARRGGBB).
struct rgb8 {
    std::uint32_t r = 0;
    std::uint32_t g = 0;
    std::uint32_t b = 0;
};
rgb8 channels(std::uint32_t pixel) {
    return {.r = (pixel >> 16u) & 0xffu, .g = (pixel >> 8u) & 0xffu, .b = pixel & 0xffu};
}

// Renders examples/NAME.ir.json offline on a 1280 × 720 frame for
// @p seconds, passing each frame, @p fps a second, to @p look; returns the
// audio of output 0. Five video pipes cover every example (landscape's).
std::vector<float> renderExample(const std::string& name, double seconds, double fps,
                                 const server::frameSink& look) {
    primitives::engine e(
        {.audio = {.sampleRate = rate, .blockSize = 256, .frameWidth = 1280, .frameHeight = 720},
         .pipes = {.video = 5}});
    const auto frames = static_cast<std::size_t>(seconds * rate);
    return server::renderOffline(e, e.build(exampleGraph(name), 1), frames, 1, fps, look).front();
}

} // namespace

TEST_CASE("monosynth plays MIDI notes, follows the filter controller, and releases") {
    example e("monosynth");
    e.run(rate / 10);
    CHECK(peak(e.out(0)) == 0.0);  // silent until a key

    e.midi(0xb0, 74, 127);  // filter open
    e.midi(0x90, 57, 100);  // A3: 220 Hz
    const std::size_t start = e.now();
    e.run(rate / 2);
    const std::size_t held = e.now();
    CHECK(rms(e.out(0), start + rate / 10, held) > 0.02);
    CHECK(peak(e.out(0)) < 1.0);
    CHECK(rms(e.out(0), start + rate / 10, held) == Approx(rms(e.out(1), start + rate / 10, held)).epsilon(1e-3));
    // The fundamental: about 220 upward crossings a second.
    CHECK(std::abs(support::crossings(e.out(0), start + rate / 10, held) - 88) < 15);

    e.midi(0x80, 57, 0);
    e.run(rate / 2);
    CHECK(peak(e.out(0), e.now() - rate / 10) < 1e-4);  // released
    CHECK(finite(e.out(0)));
}

TEST_CASE("reverb rings after an impulse, differs between sides, and decays") {
    example e("reverb");
    e.run(4 * rate, impulse(4 * rate));
    const auto& l = e.out(0);
    const auto& r = e.out(1);
    CHECK(rms(l, rate / 10, rate / 2) > 1e-3);           // a tail after the dry impulse
    CHECK(rms(l, std::size_t{3} * rate, std::size_t{4} * rate) < 1e-4);  // gone after three seconds
    double diff = 0.0;
    for (std::size_t i = rate / 10; i < static_cast<std::size_t>(rate / 2); ++i) diff += std::abs(l[i] - r[i]);
    CHECK(diff > 0.1);                                    // the sides are decorrelated
    CHECK(finite(l));
}

TEST_CASE("reverb stays bounded on full-scale noise") {
    example e("reverb");
    primitives::rng random(1);
    std::vector<float> noise(std::size_t{2} * rate);
    for (auto& v : noise) v = random.bipolar();
    e.run(2 * rate, noise);
    CHECK(peak(e.out(0)) < 2.0);
    CHECK(rms(e.out(0)) < 1.0);
}

TEST_CASE("delay repeats every 375 ms, quieter and darker each time") {
    example e("delay");
    const std::size_t t0 = e.now();  // the impulse's sample
    e.run(2 * rate, impulse(2 * rate));
    const auto& l = e.out(0);
    auto strongest = [&](std::size_t from, std::size_t to) {
        std::size_t at = from;
        for (std::size_t i = from; i < to; ++i) {
            if (std::abs(l[i]) > std::abs(l[at])) at = i;
        }
        return at;
    };
    const std::size_t first = strongest(rate / 10, rate / 2);
    const std::size_t second = strongest(rate / 2, rate * 9 / 10);
    // 375 ms, give or take the 2 ms of wow, after the dry impulse at 0.
    CHECK(std::abs(static_cast<double>(first - t0) / rate - 0.375) < 0.003);
    CHECK(std::abs(static_cast<double>(second - first) / rate - 0.375) < 0.003);
    CHECK(std::abs(l[second]) < std::abs(l[first]));
    CHECK(peak(e.out(1)) > 0.5);  // both sides carry the dry signal
    CHECK(finite(l));
}

TEST_CASE("orbit plays a tone and draws its circle where the LFO starts it") {
    // At the start the LFOs put the circle at the right of its orbit,
    // (940, 360), and the background is near black.
    std::uint32_t center = 0;
    std::uint32_t corner = 0xffffffffu;
    const auto audio = renderExample("orbit", 0.1, 10.0, [&](std::size_t index, const primitives::frame& f) {
        if (index != 0) return;
        center = f.at(940, 360);
        corner = f.at(100, 100);
    });
    CHECK(rms(audio) > 0.05f);  // the quiet sine
    CHECK(channels(center).b > 0x40u);
    CHECK(channels(corner).b < 0x20u);
}

TEST_CASE("landscape draws its layers in call order") {
    std::vector<std::uint32_t> at;
    renderExample("landscape", 0.01, 100.0, [&](std::size_t index, const primitives::frame& f) {
        if (index == 0) at = {f.at(960, 220), f.at(100, 100), f.at(420, 300), f.at(640, 700)};
    });
    REQUIRE(at.size() == 4);
    const auto sun = channels(at[0]);
    const auto sky = channels(at[1]);
    const auto mountain = channels(at[2]);
    const auto ground = channels(at[3]);
    CHECK((sun.r > 0xe0u && sun.b < 0x60u));              // yellow, over the sky
    CHECK((sky.b > sky.r && sky.b > 0xc0u));              // light blue
    CHECK((mountain.b < sky.b && mountain.r < sky.r));    // the far mountain, over the sky
    CHECK((ground.g > ground.r && ground.g > ground.b));  // green, over the mountains
}

TEST_CASE("spin turns its square and plays a tone") {
    // (790, 360) lies 150 pixels right of the center: outside the
    // 240-pixel square when it is square to the frame, inside when it has
    // turned about 45 degrees. In one second it turns a quarter turn.
    int inside = 0;
    int outside = 0;
    const auto audio = renderExample("spin", 1.0, 10.0, [&](std::size_t, const primitives::frame& f) {
        const auto p = channels(f.at(790, 360));
        ++((p.r > 0xc0u && p.b < 0x60u) ? inside : outside);
    });
    CHECK(rms(audio) > 0.05f);
    CHECK(inside > 0);
    CHECK(outside > 0);
}

TEST_CASE("meter's bar rises with the swell of its tone") {
    // The swell starts at 0 (the bar's top at y 360) and peaks at 0.5 s
    // (the top at y 60).
    std::vector<std::uint32_t> high;
    std::uint32_t low = 0;
    const auto audio = renderExample("meter", 0.6, 10.0, [&](std::size_t index, const primitives::frame& f) {
        high.push_back(f.at(640, 200));
        if (index == 0) low = f.at(640, 500);
    });
    REQUIRE(high.size() == 6);
    CHECK(rms(audio) > 0.05f);
    CHECK(channels(low).g > 0xc0u);      // the lower half is lit at once
    CHECK(channels(high[0]).g < 0x40u);  // the top is not yet
    CHECK(channels(high[5]).g > 0xc0u);  // at the swell's peak it is
}

TEST_CASE("ensemble plays its three parts from the standard library, in stereo, without a device") {
    support::renderer r(2, rate, 256);
    r.play(exampleGraph("ensemble"));
    r.run(4 * rate);
    const auto& l = r.out(0);
    CHECK(finite(l));
    CHECK(peak(l) < 1.0);
    // Sound from the first beat on; the reverb keeps the gaps between notes from silence.
    for (std::size_t at = rate / 10; at + rate / 2 <= std::size_t{4} * rate; at += rate / 2) {
        CHECK(rms(l, at, at + rate / 2) > 0.02);
    }
    CHECK(l == r.out(1));
    // The voices: 2 subtractive (a ladder each), 6 pluck (a noise burst
    // each), and 3 FM (two sines each), all built in std/instruments.lisp.
    const auto g = exampleGraph("ensemble");
    const auto count = [&](const std::string& type) {
        return std::ranges::count_if(g.nodes, [&](const metadata::nodeSpec& n) {
            return n.type == type && n.src && n.src->file == "std/instruments.lisp";
        });
    };
    CHECK(count("ladder") == 2);
    CHECK(count("noise") == 6);
    CHECK(count("sineOsc") == 6);
}
