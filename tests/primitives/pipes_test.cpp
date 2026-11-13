#include <catch2/catch_test_macros.hpp>

#include <proj/metadata/primitiveSpec.h>
#include <proj/primitives/event.h>
#include <proj/primitives/pipeTable.h>
#include <proj/primitives/plan.h>
#include <proj/primitives/spscQueue.h>

#include <vector>

using namespace primitives;

TEST_CASE("every metadata type has a factory") {
    CHECK(registeredTypes().size() == metadata::primitiveSpecs().size());
}

TEST_CASE("spsc queue keeps order and reports full and empty") {
    spscQueue<int> q(3);  // rounds up to 4
    CHECK(q.capacity() == 4);
    for (int i = 0; i < 4; ++i) CHECK(q.push(i));
    CHECK_FALSE(q.push(99));
    int v = -1;
    for (int i = 0; i < 4; ++i) {
        REQUIRE(q.pop(v));
        CHECK(v == i);
    }
    CHECK_FALSE(q.pop(v));
}

TEST_CASE("MIDI bytes convert to events and back") {
    const std::uint8_t on[] = {0x93, 60, 100};
    const auto e = fromMidi(on, 5);
    REQUIRE(e);
    CHECK(e->type == eventType::noteOn);
    CHECK(e->sampleOffset == 5);
    CHECK(channel(*e) == 3);
    CHECK(data1(*e) == 60);
    CHECK(data2(*e) == 100);
    std::array<std::uint8_t, 3> bytes{};
    CHECK(toMidi(*e, bytes) == 3);
    CHECK(bytes == std::array<std::uint8_t, 3>{0x93, 60, 100});

    const std::uint8_t silentOn[] = {0x90, 60, 0};
    CHECK(fromMidi(silentOn, 0)->type == eventType::noteOff);
    const std::uint8_t program[] = {0xc1, 7};
    CHECK(fromMidi(program, 0)->type == eventType::programChange);
    CHECK(toMidi(*fromMidi(program, 0), bytes) == 2);
    const std::uint8_t sysex[] = {0xf0, 1, 2};
    CHECK_FALSE(fromMidi(sysex, 0));
    CHECK(bendValue(pitchBend(0, 0, 12345)) == 12345);
    CHECK(toMidi(trigger(0), bytes) == 0);
}

TEST_CASE("OSC helpers read addresses and numbers without allocating") {
    std::array<std::byte, 64> buffer{};
    const auto size = encodeOscFloat(buffer, "/cutoff", 0.25f);
    REQUIRE(size == 16);
    const std::span<const std::byte> m(buffer.data(), size);
    CHECK(oscAddress(m) == "/cutoff");
    CHECK(oscFirstNumber(m) == 0.25f);
    CHECK(encodeOscFloat(std::span(buffer).first(8), "/cutoff", 1.0f) == 0);
    CHECK(oscAddress(std::span<const std::byte>{}).empty());
}

TEST_CASE("event pipes merge by offset and count overflows") {
    std::atomic<std::uint64_t> overflows{0};
    ePipe p(3, 16, &overflows);
    CHECK(p.write(trigger(10, 1.0f)));
    CHECK(p.write(trigger(2, 2.0f)));
    CHECK(p.write(trigger(10, 3.0f)));  // equal offsets keep writer order
    CHECK_FALSE(p.write(trigger(0)));
    CHECK(overflows == 1);
    const auto events = p.events();
    REQUIRE(events.size() == 3);
    CHECK(number(events[0]) == 2.0f);
    CHECK(number(events[1]) == 1.0f);
    CHECK(number(events[2]) == 3.0f);

    p.beginBlock();
    CHECK(p.events().empty());
    std::array<std::byte, 64> m{};
    const auto size = encodeOscFloat(m, "/a", 1.0f);  // 12 bytes
    CHECK(p.writeOsc(0, std::span(m).first(size)));
    CHECK_FALSE(p.writeOsc(0, std::span(m).first(size)));  // the pool holds 16 bytes
    CHECK(overflows == 2);
    CHECK(oscAddress(p.data(oscData(p.events()[0]))) == "/a");
}

TEST_CASE("audio pipes sum writers and feedback pipes delay one block") {
    aPipe p(4);
    p.configure(1, false);
    p.beginBlock(4);
    const std::vector<float> ones(4, 1.0f);
    p.add(0, ones, 0.5f);
    p.add(0, ones, 0.25f);
    CHECK(p.read(0)[3] == 0.75f);
    p.addRamp(0, ones, 0.0f, 1.0f);
    CHECK(p.read(0)[0] == 0.75f);
    CHECK(p.read(0)[2] == 0.75f + 0.5f);

    aPipe fb(4);
    fb.configure(1, true);
    fb.beginBlock(4);
    fb.add(0, ones);
    CHECK(fb.read(0)[0] == 0.0f);  // this block reads the previous one
    fb.endBlock();
    fb.beginBlock(4);
    CHECK(fb.read(0)[0] == 1.0f);
}

TEST_CASE("control pipes hold values; feedback publishes at block end") {
    cPipe c;
    c.set(0, 3.0f);
    CHECK(c.value() == 3.0f);
    c.configure(1, true);
    c.set(0, 5.0f);
    CHECK(c.value() == 3.0f);
    c.endBlock();
    CHECK(c.value() == 5.0f);
}

TEST_CASE("the pipe table starts the tempo pipe at 120 BPM and bounds indices") {
    pipeTable t({4, 4, 4, 8, 64}, {48000.0, 16});
    CHECK(t.control(metadata::tempoPipe).value() == 120.0f);
    CHECK(t.contains({metadata::pipeKind::audio, 3}));
    CHECK_FALSE(t.contains({metadata::pipeKind::audio, 4}));
    CHECK_FALSE(t.contains({metadata::pipeKind::video, 0}));
    CHECK(t.find({metadata::pipeKind::event, -1}) == nullptr);
}
