// The M6 examples play as described (io.md §15.1): each piece runs on the
// engine, its sources fed through the hub as their bridges would feed them.
// No device opens and no request is made.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/io.h>
#include <espresso/protocol/osc.h>

#include "pageMessages.h"
#include "support/examples.h"
#include "support/graph.h"
#include "support/pages.h"

using Catch::Approx;
using primitives::dataFrame;
using primitives::portKind;

namespace {

constexpr int rate = 48000;
constexpr std::size_t second = rate;
constexpr std::uint16_t pagePort = 8000;

// Plays examples/NAME/NAME.lisp with silence on the device inputs,
// one block in, so its plan is active.
class piece : public support::examplePiece {
public:
    explicit piece(const std::string& name) : examplePiece(name) { run(256); }

    // The ID of the node of @p type whose text list parameter @p param
    // holds @p entry, or the first of the type when @p entry is empty.
    std::string node(std::string_view type, std::string_view param = {}, std::string_view entry = {}) const {
        for (const auto& n : graph().nodes) {
            if (n.type != type) continue;
            if (entry.empty()) return n.id;
            const auto it = n.params.find(param);
            if (it == n.params.end()) continue;
            const auto list = metadata::as<std::vector<std::string>>(it->second);
            if (list && std::ranges::find(list->get(), entry) != list->get().end()) return n.id;
        }
        FAIL("no node of type " << type);
        return {};
    }

    // An OSC message from a page, as the page bridge delivers it.
    void page(const protocol::message& m) { support::deliverPage(io(), pagePort, m); }

    // The values of the /level messages sent to the page port since the
    // last call.
    std::vector<float> levels() {
        std::vector<float> out;
        io().drainOscOut([&](std::uint16_t port, const primitives::oscPacket& p) {
            if (port != pagePort) return;
            const auto m = protocol::decode(server::bytesOf(p.used()));
            if (m && m->address == "/level" && m->float32(0)) out.push_back(*m->float32(0));
        });
        return out;
    }
};

dataFrame frameOf(std::initializer_list<float> values) {
    dataFrame f;
    for (const float v : values) f.values[f.count++] = v;
    return f;
}

// The RMS of channel @p channel over the last @p frames rendered.
double recent(const piece& p, std::size_t frames, int channel = 0) {
    const auto& x = p.out(channel);
    return support::rms(x, x.size() - frames, x.size());
}

// The amplitude of the component at @p hz over the last @p frames.
double partial(const piece& p, double hz, std::size_t frames) {
    const auto& x = p.out(0);
    return support::amplitude(std::span(x).last(frames), hz / rate);
}

double recentPitch(const piece& p, std::size_t frames) {
    const auto& x = p.out(0);
    return support::pitch(x, x.size() - frames, x.size(), rate);
}

}  // namespace

TEST_CASE("serial-knob's knob opens its filter (io.md 15.1)") {
    piece p("serial-knob");
    p.run(second / 2);
    const double dark = partial(p, 55.0 * 36, second / 4);  // the 36th harmonic, near 2 kHz
    // The board prints 1023: the cutoff rises to 4 kHz.
    p.io().deliverSerial("", frameOf({1023.0f}));
    p.run(second / 2);
    const double bright = partial(p, 55.0 * 36, second / 4);
    CHECK(bright > 4.0 * dark);
    CHECK(recent(p, second / 4) > 0.01);
}

TEST_CASE("weather's temperature sets its pitch and its wind raises its noise (io.md 15.1)") {
    piece p("weather");
    const auto temp = p.node("webIn", "fields", "/current/temperature_2m");
    const auto wind = p.node("webIn", "fields", "/current/wind_speed_10m");
    CHECK(temp != wind);
    // 20 degrees: 110 Hz times 8^(40/60), 440 Hz; calm.
    p.io().deliverWeb(portKind::webIn, temp, frameOf({20.0f}));
    p.io().deliverWeb(portKind::webIn, wind, frameOf({0.0f}));
    p.run(12 * second);  // the 2 s glide is a time constant: within 1% after 10 s
    CHECK(recentPitch(p, second / 2) == Approx(440.0).epsilon(0.02));
    const double calm = recent(p, second / 2);
    p.io().deliverWeb(portKind::webIn, wind, frameOf({60.0f}));
    p.run(12 * second);
    CHECK(recent(p, second / 2) > 1.4 * calm);  // a gale is about as loud as the tone
}

TEST_CASE("edits plays a note for each edit and nothing without one (io.md 15.1)") {
    piece p("edits");
    p.run(second / 2);
    CHECK(recent(p, second / 2) < 1e-4);
    p.io().deliverWeb(portKind::webStream, p.node("webStream"), frameOf({50000.0f}));
    p.run(second / 4);
    CHECK(recent(p, second / 4) > 0.002);
}

TEST_CASE("firehose plays a note for each post (io.md 15.1)") {
    piece p("firehose");
    p.run(second / 2);
    CHECK(recent(p, second / 2) < 1e-4);
    p.io().deliverWeb(portKind::webSocket, p.node("webSocket"), frameOf({1.7e15f}));
    p.run(second / 4);
    CHECK(recent(p, second / 4) > 0.001);
}

TEST_CASE("report posts its loudness at the start and every four beats (io.md 15.1)") {
    piece p("report");
    std::vector<float> sent;
    for (int bar = 0; bar < 3; ++bar) {
        p.run(2 * second - (bar == 2 ? 1024 : 0));  // four beats at 120 BPM, stopping short of the fourth
        p.io().drainWebOut([&](const primitives::webPort& port, const dataFrame& f) {
            CHECK(port.request().fields == std::vector<std::string>{"loudness"});
            CHECK(port.request().url == "http://localhost:8080/loudness");
            sent.push_back(f.values[0]);
        });
    }
    REQUIRE(sent.size() == 3);  // beats 0, 4, and 8
    for (const float loudness : sent) CHECK(loudness > 0.0f);
}

TEST_CASE("page-keys plays the page's notes through its cutoff and sends its level (io.md 15.1)") {
    piece p("page-keys");
    REQUIRE(p.graph().pagePort == pagePort);
    p.page({.address = "/cutoff", .args = {0.8f}});
    p.page({.address = "/note", .args = {60.0f, 100.0f}});
    p.run(second / 2);
    CHECK(recent(p, second / 4) > 0.005);
    const auto levels = p.levels();
    REQUIRE_FALSE(levels.empty());
    CHECK(*std::ranges::max_element(levels) > 0.005f);
    p.page({.address = "/note", .args = {60.0f, 0.0f}});
    p.run(3 * second);
    CHECK(recent(p, second / 4) < 1e-3);
}

namespace {

// The panel's values when it connects (ui/index.html).
void panel(piece& p) {
    for (const auto& m : support::pageMessages(support::miniClonePanel())) p.page(m);
}

}  // namespace

TEST_CASE("mini-clone plays a key at its pitch from the panel's controls and releases it (io.md 14.5)") {
    piece p("mini-clone");
    REQUIRE(p.graph().pagePort == pagePort);
    panel(p);
    p.run(second / 4);
    CHECK(recent(p, second / 4) < 1e-4);                    // silent until a key
    p.midi(0x90, 57, 100);                                  // A3
    p.run(second / 2);
    CHECK(recent(p, second / 4) > 0.01);
    CHECK(recentPitch(p, second / 4) == Approx(220.0).epsilon(0.02));
    const auto levels = p.levels();
    REQUIRE_FALSE(levels.empty());
    CHECK(*std::ranges::max_element(levels) > 0.01f);
    p.midi(0x80, 57, 0);
    p.run(second);
    CHECK(recent(p, second / 4) < 1e-3);  // released over the 180 ms decay
}

TEST_CASE("mini-clone's range selector moves an oscillator by octaves and the A-440 switch sounds A") {
    piece p("mini-clone");
    panel(p);
    for (const auto* osc : {"/mix/osc2/on", "/mix/osc3/on"}) p.page({.address = osc, .args = {0.0f}});
    p.page({.address = "/osc1/range", .args = {4.0f}});  // 4': an octave up
    p.midi(0x90, 57, 100);
    p.run(second / 2);
    CHECK(recentPitch(p, second / 4) == Approx(440.0).epsilon(0.02));
    p.midi(0x80, 57, 0);
    p.page({.address = "/a440", .args = {1.0f}});
    p.run(second);
    CHECK(recentPitch(p, second / 4) == Approx(440.0).epsilon(0.01));
}

TEST_CASE("mini-clone's pitch wheel bends half an octave and its main output switch silences it") {
    piece p("mini-clone");
    panel(p);
    for (const auto* osc : {"/mix/osc2/on", "/mix/osc3/on"}) p.page({.address = osc, .args = {0.0f}});
    p.midi(0xe0, 0x7f, 0x7f);  // the pitch wheel all the way up
    p.midi(0x90, 57, 100);
    p.run(second / 2);
    CHECK(recentPitch(p, second / 4) == Approx(220.0 * std::sqrt(2.0)).epsilon(0.02));
    p.page({.address = "/main", .args = {0.0f}});
    p.run(second / 4);
    CHECK(recent(p, second / 8) < 1e-4);
}

namespace {

// The rack's knobs as its page starts them (examples/rack/ui/index.html),
// in their units.
void rackKnobs(piece& p) {
    for (const auto& m : support::pageMessages(support::rackKnobs())) p.page(m);
}

// Plugs (or with @p on false, pulls) the cable from @p from to @p to.
void cable(piece& p, std::string_view from, std::string_view to, bool on = true) {
    p.page({.address = support::rackCable(from, to), .args = {on ? 1.0f : 0.0f}});
}

}  // namespace

TEST_CASE("the rack plays a key through its default patch, and pulling the output's cables silences it") {
    piece p("rack");
    REQUIRE(p.graph().pagePort == pagePort);
    rackKnobs(p);
    for (const auto& [from, to] : support::rackKeyPatch()) cable(p, from, to);
    p.run(second / 4);
    CHECK(recent(p, second / 4) < 1e-4);  // silent until a key
    p.midi(0x90, 57, 100);                // A3
    p.run(second / 2);
    CHECK(recent(p, second / 4) > 0.01);
    CHECK(recent(p, second / 4, 1) > 0.01);
    CHECK(recentPitch(p, second / 4) == Approx(220.0).epsilon(0.02));
    const auto levels = p.levels();
    REQUIRE_FALSE(levels.empty());
    CHECK(*std::ranges::max_element(levels) > 0.005f);
    cable(p, "delay-out", "out-l", false);
    cable(p, "delay-out", "out-r", false);
    p.run(second / 4);
    CHECK(recent(p, second / 8) < 1e-6);
    CHECK(recent(p, second / 8, 1) < 1e-6);
}

TEST_CASE("the rack's LFO clock gates an envelope through a gate cable, with no key") {
    piece p("rack");
    rackKnobs(p);
    p.page({.address = "/lfo/rate", .args = {4.0f}});
    for (const auto& [from, to] :
         std::vector<std::pair<std::string_view, std::string_view>>{{"vco1-saw", "mix-1"},
                                                                    {"mix-out", "vcf-in"},
                                                                    {"vcf-low", "vca-in"},
                                                                    {"env1-out", "vca-cv"},
                                                                    {"vca-out", "out-l"}})
        cable(p, from, to);
    p.run(second);
    CHECK(recent(p, second / 4) < 1e-4);  // the envelope waits for a gate
    cable(p, "lfo-clock", "env1-gate");
    p.run(second);
    CHECK(recent(p, second / 2) > 0.005);
    CHECK(recentPitch(p, second / 2) == Approx(261.63).epsilon(0.03));  // VCO 1 at 8' with no pitch input
}

TEST_CASE("a rack cable back to an earlier module passes a block later and stays finite") {
    piece p("rack");
    rackKnobs(p);
    p.page({.address = "/vca/level", .args = {1.0f}});
    p.page({.address = "/vco1/fm", .args = {0.5f}});
    // VCO 2 (later in the rack) modulates VCO 1 (earlier): a feedback pipe.
    for (const auto& [from, to] : std::vector<std::pair<std::string_view, std::string_view>>{
             {"vco2-tri", "vco1-fm"}, {"vco1-saw", "vca-in"}, {"vca-out", "out-l"}, {"vca-out", "vco2-fm"}})
        cable(p, from, to);
    p.run(second);
    const auto& x = p.out(0);
    CHECK(std::ranges::all_of(x, [](float v) { return std::isfinite(v); }));
    CHECK(recent(p, second / 4) > 0.01);
}
