// The M6 examples play as described (io.md §15.1): each IR file runs on the
// engine, its sources fed through the hub as their bridges would feed them.
// No device opens and no request is made.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/io.h>
#include <espresso/protocol/osc.h>

#include "support/graph.h"
#include "support/unwrap.h"

using Catch::Approx;
using primitives::dataFrame;
using primitives::portKind;

namespace {

constexpr int rate = 48000;
constexpr std::size_t second = rate;
constexpr std::uint16_t pagePort = 8000;

// Plays examples/NAME/NAME.ir.json with nothing on the device inputs.
class piece {
public:
    explicit piece(const std::string& name) : r_(2, rate, 256) {
        std::ifstream in(std::filesystem::path(ESPRESSO_SOURCE_DIR) / "examples" / name /
                         (name + ".ir.json"));
        std::stringstream text;
        text << in.rdbuf();
        const auto ir = ir::read(text.str());
        INFO(ir.text());
        REQUIRE(ir.ok());
        graph_ = support::unwrap(ir.graph);
        r_.play(graph_, 1);
        r_.run(256);
    }

    void run(std::size_t frames) {
        r_.run(static_cast<int>(frames));
        clock_ += frames;
    }
    std::size_t clock() const noexcept { return clock_; }
    const std::vector<float>& out(int channel = 0) const { return r_.out(channel); }
    primitives::ioHub& io() { return r_.engine.io(); }
    const metadata::graphSpec& graph() const noexcept { return graph_; }

    // The ID of the node of @p type whose text list parameter @p param
    // holds @p entry, or the first of the type when @p entry is empty.
    std::string node(std::string_view type, std::string_view param = {}, std::string_view entry = {}) const {
        for (const auto& n : graph_.nodes) {
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
    void page(const protocol::message& m) {
        const auto bytes = protocol::encode(m);
        primitives::oscPacket p;
        p.size = static_cast<std::uint16_t>(bytes.size());
        std::ranges::transform(bytes, p.bytes.begin(), [](std::uint8_t b) { return std::byte{b}; });
        io().deliverOsc(pagePort, p);
    }

    // The values of the /level messages sent to the page port since the
    // last call.
    std::vector<float> levels() {
        std::vector<float> out;
        io().drainOscOut([&](std::uint16_t port, const primitives::oscPacket& p) {
            if (port != pagePort) return;
            std::vector<std::uint8_t> bytes(p.size);
            std::ranges::transform(std::span(p.bytes).first(p.size), bytes.begin(),
                                   [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
            const auto m = protocol::decode(bytes);
            if (m && m->address == "/level" && m->float32(0)) out.push_back(*m->float32(0));
        });
        return out;
    }

private:
    support::renderer r_;
    metadata::graphSpec graph_;
    std::size_t clock_ = 0;
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
    const std::vector<std::pair<std::string, float>> controls = {{"/power", 1},
                                                                 {"/volume", 0.7f},
                                                                 {"/tune", 0},
                                                                 {"/glide", 0},
                                                                 {"/modmix", 0},
                                                                 {"/osc/mod", 0},
                                                                 {"/osc3/control", 1},
                                                                 {"/osc1/range", 3},
                                                                 {"/osc1/wave", 2},
                                                                 {"/osc2/range", 3},
                                                                 {"/osc2/freq", 0.1f},
                                                                 {"/osc2/wave", 2},
                                                                 {"/osc3/range", 3},
                                                                 {"/osc3/freq", -0.1f},
                                                                 {"/osc3/wave", 3},
                                                                 {"/mix/osc1", 0.8f},
                                                                 {"/mix/osc1/on", 1},
                                                                 {"/mix/ext", 0},
                                                                 {"/mix/ext/on", 0},
                                                                 {"/mix/osc2", 0.6f},
                                                                 {"/mix/osc2/on", 1},
                                                                 {"/mix/noise", 0.3f},
                                                                 {"/mix/noise/on", 0},
                                                                 {"/noise/pink", 0},
                                                                 {"/mix/osc3", 0.4f},
                                                                 {"/mix/osc3/on", 1},
                                                                 {"/filter/mod", 0},
                                                                 {"/filter/kb1", 1},
                                                                 {"/filter/kb2", 0},
                                                                 {"/filter/cutoff", 0.5f},
                                                                 {"/filter/emphasis", 0.3f},
                                                                 {"/filter/amount", 2.5f},
                                                                 {"/filter/attack", 0.006f},
                                                                 {"/filter/decay", 0.24f},
                                                                 {"/filter/sustain", 0.3f},
                                                                 {"/amp/attack", 0.006f},
                                                                 {"/amp/decay", 0.18f},
                                                                 {"/amp/sustain", 0.8f},
                                                                 {"/a440", 0},
                                                                 {"/glide/on", 1},
                                                                 {"/decay/on", 1},
                                                                 {"/bend", 0},
                                                                 {"/modwheel", 0}};
    for (const auto& [address, value] : controls) p.page({.address = address, .args = {value}});
}

}  // namespace

TEST_CASE("mini-clone plays a key at its pitch from the panel's controls and releases it (io.md 14.5)") {
    piece p("mini-clone");
    REQUIRE(p.graph().pagePort == pagePort);
    panel(p);
    p.run(second / 4);
    CHECK(recent(p, second / 4) < 1e-4);                    // silent until a key
    p.page({.address = "/note", .args = {57.0f, 100.0f}});  // A3
    p.run(second / 2);
    CHECK(recent(p, second / 4) > 0.01);
    CHECK(recentPitch(p, second / 4) == Approx(220.0).epsilon(0.02));
    const auto levels = p.levels();
    REQUIRE_FALSE(levels.empty());
    CHECK(*std::ranges::max_element(levels) > 0.01f);
    p.page({.address = "/note", .args = {57.0f, 0.0f}});
    p.run(second);
    CHECK(recent(p, second / 4) < 1e-3);  // released over the 180 ms decay
}

TEST_CASE("mini-clone's range selector moves an oscillator by octaves and the A-440 switch sounds A") {
    piece p("mini-clone");
    panel(p);
    for (const auto* osc : {"/mix/osc2/on", "/mix/osc3/on"}) p.page({.address = osc, .args = {0.0f}});
    p.page({.address = "/osc1/range", .args = {4.0f}});  // 4': an octave up
    p.page({.address = "/note", .args = {57.0f, 100.0f}});
    p.run(second / 2);
    CHECK(recentPitch(p, second / 4) == Approx(440.0).epsilon(0.02));
    p.page({.address = "/note", .args = {57.0f, 0.0f}});
    p.page({.address = "/a440", .args = {1.0f}});
    p.run(second);
    CHECK(recentPitch(p, second / 4) == Approx(440.0).epsilon(0.01));
}
