// The M5 examples play as described (samples.md §16): each piece runs on
// an engine whose sample loader reads the example's own samples/ folder.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

#include "support/bits.h"
#include "support/examples.h"
#include "support/graph.h"

using Catch::Approx;
using support::pitch;
using support::rms;

namespace {

constexpr int rate = 48000;
constexpr std::size_t second = rate;

// Plays examples/NAME/NAME.lisp with its samples.
using piece = support::examplePiece;

// The fraction of the energy of @p n samples from @p from that lies above
// @p cutoff hertz, by a direct DFT.
double energyAbove(const std::vector<float>& x, std::size_t from, std::size_t n, double cutoff) {
    double above = 0.0;
    double total = 0.0;
    for (std::size_t k = 1; k < n / 2; ++k) {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t t = 0; t < n; ++t) {
            const double w = 2.0 * std::numbers::pi * static_cast<double>(k * t) / static_cast<double>(n);
            re += x[from + t] * std::cos(w);
            im -= x[from + t] * std::sin(w);
        }
        const double e = (re * re) + (im * im);
        total += e;
        if (static_cast<double>(k) * rate / static_cast<double>(n) > cutoff) above += e;
    }
    return above / total;
}

}  // namespace

TEST_CASE("spectral-audioscape renders the same for one seed and keeps sounding (samples.md 16.1)") {
    piece first("spectral-audioscape");
    piece again("spectral-audioscape");
    first.run(12 * second);
    again.run(12 * second);
    CHECK(support::bitDifference(again.out(0), first.out(0)) == "");
    CHECK(first.out(0) == first.out(1));  // panned to the centre
    CHECK(std::ranges::all_of(first.out(0), [](float v) { return std::isfinite(v); }));
    // The piece plays only its spectral delay, whose bands' delays run from
    // 1.87 s to 4.75 s: the first echo arrives at 1.87 s and the level
    // builds through the next second, so it is checked from 3 s on.
    for (std::size_t s = 3; s < 12; s += 2) {
        INFO(s << " s");
        CHECK(rms(first.out(0), s * second, (s + 1) * second) > 0.005);
    }
}

TEST_CASE("sampler-keys plays its recording an octave up at twice the pitch") {
    const auto played = [](std::uint8_t note) {
        piece p("sampler-keys");
        p.run(256);  // the plan's MIDI input opens when it becomes active
        p.midi(0x90, note, 100);
        p.run(second / 2);
        return pitch(p.out(0), second / 10, second / 2);
    };
    const double c4 = played(60);
    CHECK(c4 == Approx(261.63).epsilon(0.02));
    CHECK(played(72) == Approx(2.0 * c4).epsilon(0.02));
}

TEST_CASE("freeze-pedal keeps playing what it heard while the pedal is down") {
    // A tone for half a second, then silence.
    const auto heard = [](bool pedal) {
        piece p("freeze-pedal");
        const auto in = support::sine(440.0, 0.5, 0.3);
        p.run(2 * second / 5, in);  // the plan is active: the pedal reaches it
        if (pedal) p.midi(0xB0, 64, 127);
        p.run(2 * second - (2 * second / 5), in);
        return rms(p.out(0), 3 * second / 2, 2 * second);
    };
    CHECK(heard(false) < 1e-3);
    CHECK(heard(true) > 0.02);
}

TEST_CASE("grain-cloud sounds across the stereo field") {
    piece p("grain-cloud");
    p.run(4 * second);
    CHECK(rms(p.out(0), second, 4 * second) > 0.01);
    double apart = 0.0;
    for (std::size_t t = second; t < 4 * second; ++t) apart += std::abs(p.out(0)[t] - p.out(1)[t]);
    CHECK(apart / static_cast<double>(3 * second) > 0.002);
}

TEST_CASE("wavetable moves from the sine table to the square and back") {
    // The position's triangle LFO at 0.1 Hz starts at the sine table and
    // reaches the square at 5 s. The chord sounds at 110 to 220 Hz: near
    // the sine tables almost nothing lies above 1 kHz; at the square, much.
    piece p("wavetable");
    p.run(6 * second);
    CHECK(rms(p.out(0), second / 10, second / 2) > 0.02);
    CHECK(energyAbove(p.out(0), second / 10, 4096, 1000.0) < 0.01);
    CHECK(energyAbove(p.out(0), 5 * second - 2048, 4096, 1000.0) > 0.03);
}
