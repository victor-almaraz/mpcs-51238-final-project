// Event generators, MIDI and OSC sources and sinks, and control math.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <proj/metadata/graph.h>
#include <proj/primitives/event.h>
#include <proj/primitives/engine.h>
#include <proj/primitives/io.h>
#include <proj/protocol/jthread.h>

#include "support/graph.h"

using Catch::Approx;
using namespace support;
using namespace primitives;

namespace {

midiPacket midi(std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
    midiPacket p;
    p.size = 3;
    p.bytes = {status, d1, d2};
    return p;
}

oscPacket oscFloat(std::string_view address, float v) {
    oscPacket p;
    p.size = static_cast<std::uint16_t>(encodeOscFloat(p.bytes, address, v));
    return p;
}

// midiIn → events(0) → @p type → control(1).
graph fromMidi(const std::string& type, std::map<std::string, metadata::paramValue, std::less<>> params = {}) {
    graph g;
    g.pipe(events(0)).pipe(control(1));
    g.node("midi", "midiIn").outputs["out"] = {events(0)};
    auto& n = g.node("reader", type);
    n.params = std::move(params);
    n.inputs["in"] = {events(0)};
    n.outputs["out"] = {control(1)};
    return g;
}

} // namespace

TEST_CASE("MIDI input reaches event readers only while the plan runs") {
    renderer r;
    r.play(fromMidi("noteToFreq").spec);
    r.engine.io().deliverMidi("Keyboard", midi(0x90, 69, 100));  // before the swap: dropped
    r.run(64);
    CHECK(r.engine.control(1) == 0.0f);
    r.engine.io().deliverMidi("Keyboard", midi(0x90, 81, 100));
    r.run(64);
    CHECK(r.engine.control(1) == Approx(880.0f));
}

TEST_CASE("event readers: gate, velocity, controller, bend") {
    SECTION("gate") {
        renderer r;
        r.play(fromMidi("gate").spec);
        r.run(64);
        r.engine.io().deliverMidi("k", midi(0x90, 60, 100));
        r.engine.io().deliverMidi("k", midi(0x90, 64, 100));
        r.engine.io().deliverMidi("k", midi(0x80, 60, 0));
        r.run(64);
        CHECK(r.engine.control(1) == 1.0f);
        r.engine.io().deliverMidi("k", midi(0x90, 64, 0));
        r.run(64);
        CHECK(r.engine.control(1) == 0.0f);
    }
    SECTION("velocity") {
        renderer r;
        r.play(fromMidi("velocity").spec);
        r.run(64);
        r.engine.io().deliverMidi("k", midi(0x90, 60, 127));
        r.run(64);
        CHECK(r.engine.control(1) == 1.0f);
    }
    SECTION("controller number and channel") {
        renderer r;
        r.play(fromMidi("controlChange", {{"cc", 74.0}, {"channel", 2.0}}).spec);
        r.run(64);
        r.engine.io().deliverMidi("k", midi(0xb1, 74, 127));  // channel 2
        r.engine.io().deliverMidi("k", midi(0xb0, 74, 0));    // channel 1: ignored
        r.engine.io().deliverMidi("k", midi(0xb1, 7, 0));     // another controller
        r.run(64);
        CHECK(r.engine.control(1) == 1.0f);
    }
    SECTION("pitch bend in semitones") {
        renderer r;
        r.play(fromMidi("pitchBend", {{"range", 12.0}}).spec);
        r.run(64);
        r.engine.io().deliverMidi("k", midi(0xe0, 0, 0x60));  // 0x60 << 7 = 12288
        r.run(64);
        CHECK(r.engine.control(1) == Approx(6.0f));
    }
}

TEST_CASE("MIDI device patterns match case-insensitively") {
    CHECK(ioHub::matches("", "Anything"));
    CHECK(ioHub::matches("launch", "Novation Launchkey 25"));
    CHECK_FALSE(ioHub::matches("korg", "Novation Launchkey 25"));
}

TEST_CASE("MIDI output sends events with device clock times") {
    graph g;
    g.pipe(events(0));
    g.node("midi", "midiIn").outputs["out"] = {events(0)};
    auto& out = g.node("out", "midiOut");
    out.params = {{"device", std::string("synth")}, {"channel", 5.0}};
    out.inputs["in"] = {events(0)};
    renderer r;
    r.play(g.spec);
    r.run(64);
    r.engine.io().deliverMidi("k", midi(0x90, 60, 90));
    r.run(64);
    std::vector<std::pair<std::string, midiPacket>> sent;
    r.engine.io().drainMidiOut([&](std::string_view device, const midiPacket& p) { sent.emplace_back(device, p); });
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].first == "synth");
    CHECK(sent[0].second.bytes == std::array<std::uint8_t, 3>{0x94, 60, 90});  // channel 5
    CHECK(sent[0].second.time >= 64.0 / 48000.0);
}

TEST_CASE("OSC input to a control value and OSC output on change") {
    graph g;
    g.pipe(control(1));
    auto& in = g.node("osc", "oscIn");
    in.params = {{"port", 9000.0}, {"address", std::string("/cutoff")}};
    in.outputs["value"] = {control(1)};
    auto& out = g.node("send", "oscOut");
    out.params = {{"port", 9001.0}, {"address", std::string("/echo")}};
    out.inputs["value"] = {control(1)};
    renderer r;
    r.play(g.spec);
    r.run(64);
    CHECK(r.engine.io().oscInputPorts() == std::vector<std::uint16_t>{9000});
    r.engine.io().deliverOsc(9000, oscFloat("/cutoff", 0.75f));
    r.engine.io().deliverOsc(9000, oscFloat("/other", 0.1f));
    r.run(64);
    CHECK(r.engine.control(1) == 0.75f);
    std::vector<float> sent;
    r.engine.io().drainOscOut([&](std::uint16_t port, const oscPacket& p) {
        CHECK(port == 9001);
        const std::span<const std::byte> m(p.bytes.data(), p.size);
        CHECK(oscAddressIs(m, "/echo"));
        sent.push_back(*oscFirstNumber(m));
    });
    CHECK(sent == std::vector<float>{0.0f, 0.75f});  // the initial value, then the change
    r.run(64);
    r.engine.io().drainOscOut([&](std::uint16_t, const oscPacket&) { FAIL("no change, no message"); });
}

TEST_CASE("OSC messages pass through event pipes and filters") {
    graph g;
    g.pipe(events(0)).pipe(events(1));
    auto& in = g.node("osc", "oscIn");
    in.params["port"] = 9100.0;
    in.outputs["out"] = {events(0)};
    auto& f = g.node("filter", "eventFilter");
    f.params = {{"type", std::string("osc")}, {"address", std::string("/keep")}};
    f.inputs["in"] = {events(0)};
    f.outputs["out"] = {events(1)};
    auto& out = g.node("send", "oscOut");
    out.params["port"] = 9101.0;
    out.inputs["in"] = {events(1)};
    renderer r;
    r.play(g.spec);
    r.run(64);
    r.engine.io().deliverOsc(9100, oscFloat("/keep", 1.0f));
    r.engine.io().deliverOsc(9100, oscFloat("/drop", 2.0f));
    r.run(64);
    int count = 0;
    r.engine.io().drainOscOut([&](std::uint16_t, const oscPacket& p) {
        CHECK(oscAddressIs(std::span(p.bytes).first(p.size), "/keep"));
        ++count;
    });
    CHECK(count == 1);
}

TEST_CASE("voiceSplit assigns voices and steals the oldest") {
    graph g;
    g.pipe(events(0)).pipe(events(1)).pipe(events(2));
    g.node("midi", "midiIn").outputs["out"] = {events(0)};
    auto& v = g.node("voices", "voiceSplit");
    v.inputs["in"] = {events(0)};
    v.outputs["out"] = {events(1), events(2)};
    renderer r;
    r.play(g.spec);
    r.run(64);
    for (std::uint8_t note : {std::uint8_t{60}, std::uint8_t{62}, std::uint8_t{64}}) r.engine.io().deliverMidi("k", midi(0x90, note, 100));
    r.run(64);
    const auto first = r.engine.pipes().events(1).events();
    const auto second = r.engine.pipes().events(2).events();
    // Voice 0: note 60, then stolen (note-off 60) for 64. Voice 1: 62.
    REQUIRE(first.size() == 3);
    CHECK((first[0].type == eventType::noteOn && data1(first[0]) == 60));
    CHECK((first[1].type == eventType::noteOff && data1(first[1]) == 60));
    CHECK((first[2].type == eventType::noteOn && data1(first[2]) == 64));
    REQUIRE(second.size() == 1);
    CHECK(data1(second[0]) == 62);
}

TEST_CASE("metro triggers on the beat clock with sample offsets") {
    graph g;
    g.pipe(events(0)).pipe(control(1));
    auto& m = g.node("metro", "metro");
    m.params["every"] = 0.5;
    m.outputs["out"] = {events(0)};
    auto& s = g.node("seq", "sequencer");
    s.params["values"] = std::vector<double>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    s.inputs["in"] = {events(0)};
    s.outputs["out"] = {control(1)};
    renderer r(1, 48000.0, 100);
    r.play(g.spec);
    // 120 BPM: a half beat every 12000 samples. Stop between triggers, so
    // rounding in the beat clock cannot move one across a run boundary.
    r.run(47950);  // triggers at 0, 12000, 24000, 36000
    CHECK(r.engine.control(1) == 4.0f);
    r.run(100);  // beat 2 is 50 samples into this block
    CHECK(r.engine.control(1) == 5.0f);
    const auto offset = r.engine.pipes().events(0).events()[0].sampleOffset;
    CHECK((offset == 49 || offset == 50));
}

TEST_CASE("sequencer orders") {
    auto run = [](const std::string& order, int steps) {
        graph g;
        g.pipe(events(0)).pipe(control(1));
        g.node("metro", "metro").outputs["out"] = {events(0)};
        auto& s = g.node("seq", "sequencer");
        s.params = {{"values", std::vector<double>{1, 2, 3}}, {"order", order}};
        s.inputs["in"] = {events(0)};
        s.outputs["out"] = {control(1)};
        renderer r(1, 48000.0, 64);
        r.play(g.spec);
        std::vector<float> seen;
        r.run(12000);  // the trigger at beat 0
        for (int i = 0; i < steps; ++i) {
            seen.push_back(r.engine.control(1));
            r.run(24000);  // one beat, across the next trigger
        }
        return seen;
    };
    CHECK(run("forward", 4) == std::vector<float>{1, 2, 3, 1});
    CHECK(run("backward", 4) == std::vector<float>{3, 2, 1, 3});
    CHECK(run("ping-pong", 6) == std::vector<float>{1, 2, 3, 2, 1, 2});
    const auto random = run("random", 8);
    CHECK(random == run("random", 8));
}

TEST_CASE("chance keeps about its probability of events") {
    graph g;
    g.pipe(events(0)).pipe(events(1)).pipe(control(1));
    auto& m = g.node("metro", "metro");
    m.params["every"] = 1.0 / 64.0;
    m.outputs["out"] = {events(0)};
    auto& c = g.node("chance", "chance");
    c.params["probability"] = 0.25;
    c.inputs["in"] = {events(0)};
    c.outputs["out"] = {events(1)};
    auto& s = g.node("count", "sequencer");
    std::vector<double> counting(10000);
    for (std::size_t i = 0; i < counting.size(); ++i) counting[i] = static_cast<double>(i + 1);
    s.params["values"] = counting;
    s.inputs["in"] = {events(1)};
    s.outputs["out"] = {control(1)};
    renderer r;
    r.play(g.spec);
    r.run(48000 * 8);  // 16 beats × 64 = 1024 triggers
    CHECK(r.engine.control(1) == Approx(256).margin(40));
}

TEST_CASE("threshold and sample and hold") {
    graph g;
    g.pipe(audio(0)).pipe(events(0)).pipe(control(1));
    auto& s = g.node("sine", "sineOsc");
    s.params["freq"] = 10.0;
    s.outputs["out"] = {audio(0)};
    auto& t = g.node("threshold", "threshold");
    t.params = {{"level", 0.5}, {"direction", std::string("rising")}};
    t.inputs["in"] = {audio(0)};
    t.outputs["out"] = {events(0)};
    auto& h = g.node("hold", "sampleAndHold");
    h.inputs = {{"trigger", {events(0)}}, {"in", {audio(0)}}};
    h.outputs["out"] = {control(1)};
    renderer r;
    r.play(g.spec);
    r.run(48000);
    CHECK(r.engine.control(1) == Approx(0.5f).margin(0.01f));
}

TEST_CASE("control math follows its output pipe type") {
    graph g;
    g.pipe(control(1)).pipe(control(2)).pipe(control(3)).pipe(control(4));
    auto& db = g.node("db", "dbToAmp");
    db.inputs["in"] = {control(1)};
    db.outputs["out"] = {control(2)};
    auto& sc = g.node("scale", "scaleExp");
    sc.params = {{"in-low", 0.0}, {"in-high", 1.0}, {"out-low", 20.0}, {"out-high", 20000.0}};
    sc.inputs["in"] = {control(2)};
    sc.outputs["out"] = {control(3)};
    auto& b = g.node("beats", "beatsToSeconds");
    b.inputs["in"] = {control(2)};
    b.outputs["out"] = {control(4)};
    renderer r;
    r.play(g.spec);
    r.engine.setControl(1, -6.0f);
    r.run(64);
    CHECK(r.engine.control(2) == Approx(0.501187f));
    CHECK(r.engine.control(3) == Approx(20.0 * std::pow(1000.0, 0.501187)).epsilon(1e-4));
    CHECK(r.engine.control(4) == Approx(0.501187f * 0.5f));  // 120 BPM: half a second per beat
}

TEST_CASE("mix levels and multiply") {
    graph g;
    g.pipe(control(1)).pipe(control(2)).pipe(control(3)).pipe(control(4));
    auto& m = g.node("mix", "mix");
    m.params["levels"] = std::vector<double>{0.5, 2.0};
    m.inputs["in"] = {control(1), control(2)};
    m.outputs["out"] = {control(3)};
    auto& p = g.node("product", "multiply");
    p.inputs["in"] = {control(1), control(2)};
    p.outputs["out"] = {control(4)};
    renderer r;
    r.play(g.spec);
    r.engine.setControl(1, 4.0f);
    r.engine.setControl(2, 3.0f);
    r.run(64);
    CHECK(r.engine.control(3) == 8.0f);
    CHECK(r.engine.control(4) == 12.0f);
}

TEST_CASE("adoption keeps state, not parameters") {
    SECTION("a new controller number applies after the swap") {
        renderer r;
        r.play(fromMidi("controlChange", {{"cc", 1.0}}).spec, 1);
        r.run(64);
        r.engine.io().deliverMidi("k", midi(0xb0, 1, 127));
        r.run(64);
        CHECK(r.engine.control(1) == 1.0f);
        r.play(fromMidi("controlChange", {{"cc", 7.0}}).spec, 2);  // same IDs and shape: adopted
        r.run(64);
        r.engine.io().deliverMidi("k", midi(0xb0, 7, 0));
        r.run(64);
        CHECK(r.engine.control(1) == 0.0f);
    }
    SECTION("a new metro interval applies after the swap") {
        auto counter = [](double every) {
            graph g;
            g.spec.tempo = 6000.0;  // 100 beats a second
            g.pipe(events(0)).pipe(control(1));
            auto& m = g.node("metro", "metro");
            m.params["every"] = every;
            m.outputs["out"] = {events(0)};
            auto& s = g.node("count", "sequencer");
            std::vector<double> values(1000);
            for (std::size_t i = 0; i < values.size(); ++i) values[i] = static_cast<double>(i + 1);
            s.params["values"] = values;
            s.inputs["in"] = {events(0)};
            s.outputs["out"] = {control(1)};
            return g.spec;
        };
        renderer r;
        r.play(counter(0.25), 1);
        r.run(48000 / 2);  // 50 beats: 200 triggers
        const float before = r.engine.control(1);
        CHECK(before == Catch::Approx(200.0f).margin(2.0f));
        r.play(counter(1.0), 2);
        r.run(48000 / 10);  // 10 beats
        CHECK(r.engine.control(1) == Catch::Approx(before + 10.0f).margin(2.0f));
    }
}

TEST_CASE("an adopted MIDI or OSC input keeps the packets queued at the swap") {
    SECTION("MIDI") {
        renderer r;
        r.play(fromMidi("gate").spec, 1);
        r.run(64);
        r.play(fromMidi("gate").spec, 2);  // built and waiting for the next block
        r.engine.io().deliverMidi("k", midi(0x90, 60, 100));
        r.run(64);  // swaps, then reads the note queued in plan 1's port
        CHECK(r.engine.control(1) == 1.0f);
        r.engine.io().deliverMidi("k", midi(0x80, 60, 0));
        r.run(64);
        CHECK(r.engine.control(1) == 0.0f);
    }
    SECTION("OSC") {
        graph g;
        g.pipe(control(1));
        auto& in = g.node("osc", "oscIn");
        in.params = {{"port", 9400.0}, {"address", std::string("/x")}};
        in.outputs["value"] = {control(1)};
        renderer r;
        r.play(g.spec, 1);
        r.run(64);
        r.play(g.spec, 2);
        r.engine.io().deliverOsc(9400, oscFloat("/x", 0.5f));
        r.run(64);
        CHECK(r.engine.control(1) == 0.5f);
    }
}

TEST_CASE("the hub delivers each message to one active port per node") {
    ioHub hub;
    const auto older = hub.openMidiIn("synth", "");
    const auto newer = hub.openMidiIn("synth", "");  // an adopting node's port, active during the swap
    const auto other = hub.openMidiIn("drums", "");
    older->setActive(true);
    newer->setActive(true);
    other->setActive(true);
    hub.deliverMidi("k", midi(0x90, 60, 100));
    midiPacket p;
    CHECK(older->queue.pop(p));
    CHECK_FALSE(newer->queue.pop(p));
    CHECK(other->queue.pop(p));
    older->setActive(false);  // the engine deactivates the outgoing plan's ports
    hub.deliverMidi("k", midi(0x80, 60, 0));
    CHECK_FALSE(older->queue.pop(p));
    CHECK(newer->queue.pop(p));
}

TEST_CASE("MIDI delivered during repeated swaps is neither lost nor doubled") {
    // midiIn → sequencer counting 1, 2, 3, …: after N note-ons the count is
    // N exactly. One thread delivers while this one renders and swaps in
    // a plan of the same shape (so every node adopts) every few blocks.
    constexpr int total = 3000;
    graph g;
    g.pipe(events(0)).pipe(control(1));
    g.node("midi", "midiIn").outputs["out"] = {events(0)};
    auto& s = g.node("count", "sequencer");
    std::vector<double> counting(total);
    for (std::size_t i = 0; i < counting.size(); ++i) counting[i] = static_cast<double>(i + 1);
    s.params["values"] = counting;
    s.inputs["in"] = {events(0)};
    s.outputs["out"] = {control(1)};
    renderer r;
    r.play(g.spec, 1);
    r.run(64);
    std::atomic<int> sent{0};
    {
        const proj::jthread producer([&](const proj::stop_token& stop) {
            while (!stop.stop_requested() && sent.load() < total) {
                // Stay well within one port's queue.
                if (sent.load() - static_cast<int>(r.engine.control(1)) > 128) {
                    std::this_thread::yield();
                    continue;
                }
                r.engine.io().deliverMidi("k", midi(0x90, 60, 100));
                sent.fetch_add(1);
            }
        });
        // Bounded: if packets were lost, the producer would wait forever.
        for (std::int32_t id = 2; sent.load() < total && id < 20000; ++id) {
            r.play(g.spec, id);
            for (int k = 0; k < 3; ++k) r.run(64);
            std::ignore = r.engine.collect();  // frees retired plans, as the control thread does
        }
    }
    for (int k = 0; k < 4; ++k) r.run(64);
    CHECK(r.engine.control(1) == static_cast<float>(total));
    CHECK(r.engine.counters().drops == 0);
}

TEST_CASE("an OSC packet whose size exceeds its buffer is clamped") {
    graph g;
    g.pipe(events(0));
    auto& in = g.node("osc", "oscIn");
    in.params["port"] = 9500.0;
    in.outputs["out"] = {events(0)};
    auto& out = g.node("send", "oscOut");
    out.params["port"] = 9501.0;
    out.inputs["in"] = {events(0)};
    renderer r;
    r.play(g.spec);
    r.run(64);
    auto p = oscFloat("/x", 1.0f);
    p.size = 5000;  // more than maxOscBytes
    r.engine.io().deliverOsc(9500, p);
    r.run(64);
    std::vector<std::size_t> sizes;
    r.engine.io().drainOscOut([&](std::uint16_t, const oscPacket& sent) {
        sizes.push_back(sent.size);
        CHECK(oscAddressIs(std::span(sent.bytes).first(sent.size), "/x"));
    });
    CHECK(sizes == std::vector<std::size_t>{maxOscBytes});
}

TEST_CASE("I/O callbacks may call the hub") {
    graph g;
    g.pipe(control(1));
    auto& in = g.node("osc", "oscIn");
    in.params["port"] = 9600.0;
    in.outputs["value"] = {control(1)};
    auto& out = g.node("send", "oscOut");
    out.params = {{"port", 9601.0}, {"address", std::string("/y")}};
    out.inputs["value"] = {control(1)};
    renderer r;
    r.play(g.spec);
    r.run(64);
    std::vector<std::uint16_t> seen;
    // Draining calls back into the hub; it must not hold its lock meanwhile.
    r.engine.io().drainOscOut([&](std::uint16_t, const oscPacket&) { seen = r.engine.io().oscInputPorts(); });
    CHECK(seen == std::vector<std::uint16_t>{9600});
}

TEST_CASE("a repeated note-on retriggers its voice") {
    graph g;
    g.pipe(events(0)).pipe(events(1)).pipe(events(2));
    g.node("midi", "midiIn").outputs["out"] = {events(0)};
    auto& v = g.node("voices", "voiceSplit");
    v.inputs["in"] = {events(0)};
    v.outputs["out"] = {events(1), events(2)};
    renderer r;
    r.play(g.spec);
    r.run(64);
    r.engine.io().deliverMidi("k", midi(0x90, 60, 100));
    r.engine.io().deliverMidi("k", midi(0x90, 60, 90));
    r.engine.io().deliverMidi("k", midi(0x80, 60, 0));
    r.run(64);
    CHECK(r.engine.pipes().events(1).events().size() == 3);
    CHECK(r.engine.pipes().events(2).events().empty());
}

TEST_CASE("a delivery that began before two swaps still reaches the node") {
    // The hub reads that a port is active, then pushes; the audio thread may
    // deactivate the port in between. A packet pushed that late, into a port
    // two plans back, still arrives.
    renderer r;
    const auto first = r.engine.build(fromMidi("gate").spec, 1);
    r.engine.play(first, 0.0, quantization::none);
    r.run(64);
    const auto port = std::static_pointer_cast<midiPort>(first->ports.front());
    {
        const ioPort::delivery late(*port);  // starts while plan 1 runs
        REQUIRE(late.admitted());
        r.play(fromMidi("gate").spec, 2);
        r.run(64);
        r.play(fromMidi("gate").spec, 3);
        r.run(64);
        REQUIRE(port->queue.push(midi(0x90, 60, 100)));
    }
    r.run(64);
    CHECK(r.engine.control(1) == 1.0f);
    std::ignore = r.engine.collect();
}
