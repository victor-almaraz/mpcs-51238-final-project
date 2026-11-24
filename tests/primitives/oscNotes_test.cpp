// oscNotes: notes from OSC messages delivered through oscIn (io.md §12.10).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/event.h>
#include <espresso/primitives/io.h>

#include "support/graph.h"

using namespace support;
using namespace primitives;

namespace {

// An OSC argument: an `i`, an `f`, or an `s`.
struct arg {
    char tag;
    float number = 0.0f;
    std::string text;
};

// OSC bytes, big-endian, each string padded to four bytes.
class encoder {
public:
    void string(std::string_view s) {
        for (const char c : s) bytes_.push_back(static_cast<std::byte>(c));
        do bytes_.push_back(std::byte{0});
        while (bytes_.size() % 4 != 0);
    }
    void big32(std::uint32_t v) {
        for (const unsigned shift : {24U, 16U, 8U, 0U})
            bytes_.push_back(static_cast<std::byte>((v >> shift) & 0xffU));
    }
    oscPacket packet() const {
        oscPacket p;
        std::ranges::copy(bytes_, p.bytes.begin());
        p.size = static_cast<std::uint16_t>(bytes_.size());
        return p;
    }

private:
    std::vector<std::byte> bytes_;
};

// An OSC message to @p address with @p args.
oscPacket message(std::string_view address, const std::vector<arg>& args) {
    encoder e;
    e.string(address);
    std::string tags = ",";
    for (const auto& a : args) tags += a.tag;
    e.string(tags);
    for (const auto& a : args) {
        if (a.tag == 'i') e.big32(static_cast<std::uint32_t>(static_cast<std::int32_t>(a.number)));
        if (a.tag == 'f') e.big32(std::bit_cast<std::uint32_t>(a.number));
        if (a.tag == 's') e.string(a.text);
    }
    return e.packet();
}

arg i(float v) { return {.tag = 'i', .number = v, .text = {}}; }
arg f(float v) { return {.tag = 'f', .number = v, .text = {}}; }
arg s(std::string v) { return {.tag = 's', .number = 0.0f, .text = std::move(v)}; }

constexpr std::uint16_t port = 8000;

// oscIn on port → events 0 → oscNotes → events 1.
graph keys(std::string address = "/note") {
    graph g;
    g.pipe(events(0)).pipe(events(1));
    auto& in = g.node("osc", "oscIn");
    in.params["port"] = static_cast<double>(port);
    in.outputs["out"] = {events(0)};
    auto& notes = g.node("notes", "oscNotes");
    if (address != "/note") notes.params["address"] = address;
    notes.inputs["in"] = {events(0)};
    notes.outputs["out"] = {events(1)};
    return g;
}

// The note events made from @p packets in one block.
std::vector<event> notesFrom(const std::vector<oscPacket>& packets, const graph& g = keys()) {
    renderer r(1, 48000.0, 64);
    r.play(g.spec);
    r.run(64);
    for (const auto& p : packets) r.engine.io().deliverOsc(port, p);
    r.run(64);
    const auto out = r.engine.pipes().events(1).events();
    return {out.begin(), out.end()};
}

}  // namespace

TEST_CASE("oscNotes turns note messages into note-ons and note-offs (io.md 12.10)") {
    const auto notes =
        notesFrom({message("/note", {i(60), i(100)}), message("/note", {f(64.4f), f(90.6f), i(10)}),
                   message("/note", {i(60), i(0)})});
    REQUIRE(notes.size() == 3);
    CHECK(notes[0].type == eventType::noteOn);
    CHECK(data1(notes[0]) == 60);
    CHECK(data2(notes[0]) == 100);
    CHECK(channel(notes[0]) == 0);  // channel 1, the default
    // Floats round; the fourth argument is the channel, 1 to 16.
    CHECK(data1(notes[1]) == 64);
    CHECK(data2(notes[1]) == 91);
    CHECK(channel(notes[1]) == 9);
    CHECK(notes[2].type == eventType::noteOff);  // velocity 0
    CHECK(data1(notes[2]) == 60);
}

TEST_CASE("oscNotes clamps notes and velocities to 0 to 127 and channels to 1 to 16") {
    const auto notes =
        notesFrom({message("/note", {i(200), i(300), i(40)}), message("/note", {f(-3.0f), f(5.0f), i(0)})});
    REQUIRE(notes.size() == 2);
    CHECK(data1(notes[0]) == 127);
    CHECK(data2(notes[0]) == 127);
    CHECK(channel(notes[0]) == 15);
    CHECK(data1(notes[1]) == 0);
    CHECK(channel(notes[1]) == 0);
}

TEST_CASE("oscNotes passes other addresses by and drops malformed note messages") {
    renderer r(1, 48000.0, 64);
    r.play(keys().spec);
    r.run(64);
    const auto drops = r.engine.io().drops();
    for (const auto& p : {message("/cutoff", {f(0.5f)}), message("/note", {i(60)}),
                          message("/note", {s("C4"), i(100)}), message("/note", {i(60), i(100), s("one")}),
                          message("/note", {f(60.0f), f(std::numeric_limits<float>::quiet_NaN())})}) {
        r.engine.io().deliverOsc(port, p);
    }
    r.run(64);
    CHECK(r.engine.pipes().events(1).events().empty());
    CHECK(r.engine.io().drops() == drops + 4);  // all but /cutoff

    // Another address names the note messages.
    const auto pads =
        notesFrom({message("/pad", {i(36), i(127)}), message("/note", {i(60), i(100)})}, keys("/pad"));
    REQUIRE(pads.size() == 1);
    CHECK(data1(pads[0]) == 36);
}
