// The example pieces play as described: their IR files run on the engine
// with MIDI and audio input.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <proj/ir/ir.h>
#include <proj/primitives/engine.h>
#include <proj/primitives/random.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "support/graph.h"

using Catch::Approx;
using support::peak;
using support::rms;

namespace {

constexpr int rate = 48000;

// Plays examples/NAME.ir.json; inputs are fed to device inputs 0 and 1.
class example {
public:
    explicit example(const std::string& name) : engine_({{double(rate), 128}, {}}) {
        std::ifstream in(std::filesystem::path(PROJ_SOURCE_DIR) / "examples" / (name + ".ir.json"));
        std::stringstream text;
        text << in.rdbuf();
        const auto r = ir::read(text.str());
        INFO(r.text());
        REQUIRE(r.ok());
        engine_.play(engine_.build(*r.graph, 1), 0.0, primitives::quantization::none);
        run(128);
    }

    void run(int frames, const std::vector<float>& input = {}) {
        const auto n = static_cast<std::size_t>(frames);
        std::vector<float> in0(n, 0.0f), in1(n, 0.0f);
        for (std::size_t i = 0; i < n && i < input.size(); ++i) in0[i] = in1[i] = input[i];
        const float* ins[] = {in0.data(), in1.data()};
        for (auto& o : out_) o.resize(o.size() + n);
        float* outs[] = {out_[0].data() + out_[0].size() - n, out_[1].data() + out_[1].size() - n};
        engine_.process({ins, 2, outs, 2}, frames, time_);
        time_ += frames / double(rate);
    }

    void midi(std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
        primitives::midiPacket p;
        p.size = 3;
        p.bytes = {status, d1, d2};
        p.time = time_;
        engine_.io().deliverMidi("Keyboard", p);
    }

    const std::vector<float>& out(int ch) const { return out_[static_cast<std::size_t>(ch)]; }
    std::size_t now() const { return out_[0].size(); }

private:
    primitives::engine engine_;
    std::array<std::vector<float>, 2> out_;
    double time_ = 0.0;
};

bool finite(const std::vector<float>& x) {
    return std::all_of(x.begin(), x.end(), [](float v) { return std::isfinite(v); });
}

std::vector<float> impulse(int frames) {
    std::vector<float> x(static_cast<std::size_t>(frames), 0.0f);
    x[0] = 1.0f;
    return x;
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
    CHECK(rms(l, 3 * rate, 4 * rate) < 1e-4);            // gone after three seconds
    double diff = 0.0;
    for (std::size_t i = rate / 10; i < static_cast<std::size_t>(rate / 2); ++i) diff += std::abs(l[i] - r[i]);
    CHECK(diff > 0.1);                                    // the sides are decorrelated
    CHECK(finite(l));
}

TEST_CASE("reverb stays bounded on full-scale noise") {
    example e("reverb");
    primitives::rng random(1);
    std::vector<float> noise(2 * rate);
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
