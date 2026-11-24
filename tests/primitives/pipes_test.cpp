#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/metadata/primitiveSpec.h>
#include <espresso/primitives/denormals.h>
#include <espresso/primitives/event.h>
#include <espresso/primitives/pipe.h>
#include <espresso/primitives/pipeTable.h>
#include <espresso/primitives/plan.h>
#include <espresso/primitives/primitive.h>
#include <espresso/primitives/spscQueue.h>

#include "support/unwrap.h"

using namespace primitives;
using support::unwrap;

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
    const std::array<std::uint8_t, 3> on{0x93, 60, 100};
    const auto e = unwrap(fromMidi(on, 5));
    CHECK(e.type == eventType::noteOn);
    CHECK(e.sampleOffset == 5);
    CHECK(channel(e) == 3);
    CHECK(data1(e) == 60);
    CHECK(data2(e) == 100);
    std::array<std::uint8_t, 3> bytes{};
    CHECK(toMidi(e, bytes) == 3);
    CHECK(bytes == std::array<std::uint8_t, 3>{0x93, 60, 100});

    const std::array<std::uint8_t, 3> silentOn{0x90, 60, 0};
    CHECK(unwrap(fromMidi(silentOn, 0)).type == eventType::noteOff);
    const std::array<std::uint8_t, 2> program{0xc1, 7};
    CHECK(unwrap(fromMidi(program, 0)).type == eventType::programChange);
    CHECK(toMidi(unwrap(fromMidi(program, 0)), bytes) == 2);
    const std::array<std::uint8_t, 3> sysex{0xf0, 1, 2};
    CHECK_FALSE(fromMidi(sysex, 0));
    CHECK(bendValue(pitchBend(0, 0, 12345)) == 12345);
    CHECK(toMidi(trigger(0), bytes) == 0);
}

TEST_CASE("MIDI bytes keep their message type whatever the payload holds") {
    // The payload is public; a channel byte above 15 must not reach the
    // status byte's type bits, nor a data byte above 127 the status bit.
    event e = noteOn(0, 0, 60, 100);
    e.payload[0] = std::byte{0x23};
    e.payload[1] = std::byte{0xbc};
    std::array<std::uint8_t, 3> bytes{};
    REQUIRE(toMidi(e, bytes) == 3);
    CHECK(bytes == std::array<std::uint8_t, 3>{0x93, 0x3c, 100});
}

TEST_CASE("OSC helpers read addresses and numbers without allocating") {
    std::array<std::byte, 64> buffer{};
    const auto size = encodeOscFloat(buffer, "/cutoff", 0.25f);
    REQUIRE(size == 16);
    const std::span<const std::byte> m(buffer.data(), size);
    CHECK(oscAddressIs(m, "/cutoff"));
    CHECK_FALSE(oscAddressIs(m, "/cut"));
    CHECK_FALSE(oscAddressIs(m, "/cutoff/x"));
    CHECK(oscFirstNumber(m) == 0.25f);
    CHECK(encodeOscFloat(std::span(buffer).first(8), "/cutoff", 1.0f) == 0);
    CHECK_FALSE(oscAddressIs(std::span<const std::byte>{}, ""));
    CHECK_FALSE(oscAddressIs(std::span(buffer).first(4), "/cut"));  // unterminated
}

TEST_CASE("event pipes merge by offset and count overflows") {
    const auto counter = std::make_shared<std::atomic<std::uint64_t>>(0);
    const auto& overflows = *counter;
    ePipe p(3, 16, counter);
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
    CHECK(oscAddressIs(p.data(oscData(p.events()[0])), "/a"));
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
    pipeTable t({.audio = 4, .control = 4, .event = 4, .eventsPerPipe = 8, .poolBytes = 64},
                {.sampleRate = 48000.0, .blockSize = 16});
    CHECK(t.control(metadata::tempoPipe).value() == 120.0f);
    CHECK(t.contains({metadata::pipeKind::audio, 3}));
    CHECK_FALSE(t.contains({metadata::pipeKind::audio, 4}));
    CHECK(t.find({metadata::pipeKind::event, -1}) == nullptr);
}

namespace {

// An OSC message built field by field, big-endian and padded (OSC 1.0).
class oscMessage {
public:
    oscMessage& text(std::string_view s) {
        for (char c : s) bytes_.push_back(static_cast<std::byte>(c));
        bytes_.push_back(std::byte{0});
        while (bytes_.size() % 4 != 0) bytes_.push_back(std::byte{0});
        return *this;
    }
    oscMessage& int32(std::uint32_t v) { return big(v, 4); }
    oscMessage& int64(std::uint64_t v) { return big(v, 8); }
    oscMessage& raw(std::size_t n) {
        bytes_.insert(bytes_.end(), n, std::byte{0});
        return *this;
    }
    std::span<const std::byte> bytes() const { return bytes_; }
    // The message without its last @p n bytes.
    std::span<const std::byte> truncated(std::size_t n) const { return bytes().first(bytes_.size() - n); }

private:
    oscMessage& big(std::uint64_t v, int count) {
        for (auto k = static_cast<std::uint64_t>(count); k-- > 0;)
            bytes_.push_back(static_cast<std::byte>((v >> (8U * k)) & 0xffU));
        return *this;
    }
    std::vector<std::byte> bytes_;
};

}  // namespace

TEST_CASE("oscFirstNumber reads every numeric tag and skips the others") {
    CHECK(oscFirstNumber(oscMessage().text("/a").text(",i").int32(static_cast<std::uint32_t>(-7)).bytes()) ==
          -7.0f);
    CHECK(oscFirstNumber(
              oscMessage().text("/a").text(",f").int32(std::bit_cast<std::uint32_t>(0.5f)).bytes()) == 0.5f);
    CHECK(oscFirstNumber(oscMessage().text("/a").text(",h").int64(static_cast<std::uint64_t>(-3)).bytes()) ==
          -3.0f);
    CHECK(oscFirstNumber(
              oscMessage().text("/a").text(",d").int64(std::bit_cast<std::uint64_t>(0.25)).bytes()) == 0.25f);
    // A string and a five-byte blob (padded to eight) before the number.
    const auto skipped = oscMessage().text("/a").text(",sbf").text("hello").int32(5).raw(8).int32(
        std::bit_cast<std::uint32_t>(2.0f));
    CHECK(oscFirstNumber(skipped.bytes()) == 2.0f);
    // No numeric argument, an unknown tag, or no tag string.
    CHECK_FALSE(oscFirstNumber(oscMessage().text("/a").text(",s").text("x").bytes()));
    CHECK_FALSE(oscFirstNumber(oscMessage().text("/a").text(",T").bytes()));
    CHECK_FALSE(oscFirstNumber(oscMessage().text("/a").bytes()));
    CHECK_FALSE(oscFirstNumber(oscMessage().text("a").text(",f").int32(0).bytes()));  // no leading slash
}

TEST_CASE("oscFirstNumber rejects truncated messages") {
    CHECK_FALSE(oscFirstNumber(oscMessage().text("/a").text(",i").int32(1).truncated(1)));
    CHECK_FALSE(oscFirstNumber(oscMessage().text("/a").text(",f").int32(1).truncated(2)));
    CHECK_FALSE(oscFirstNumber(oscMessage().text("/a").text(",h").int64(1).truncated(4)));
    CHECK_FALSE(oscFirstNumber(oscMessage().text("/a").text(",d").int64(1).truncated(1)));
    CHECK_FALSE(
        oscFirstNumber(oscMessage().text("/a").text(",sf").text("abc").truncated(4)));  // unterminated string
    CHECK_FALSE(oscFirstNumber(oscMessage().text("/abcd").truncated(4)));  // unterminated address
    CHECK_FALSE(
        oscFirstNumber(oscMessage().text("/a").text(",bf").int32(2).truncated(0)));  // blob past the end
    // A blob length near 2^32 must not wrap around to a short skip.
    const auto huge =
        oscMessage().text("/a").text(",bf").int32(0xfffffffe).int32(std::bit_cast<std::uint32_t>(1.0f));
    CHECK_FALSE(oscFirstNumber(huge.bytes()));
}

TEST_CASE("event pipes copy OSC events with their pool data") {
    auto overflows = std::make_shared<std::atomic<std::uint64_t>>(0);
    ePipe from(4, 64, overflows), to(4, 64, overflows);
    std::array<std::byte, 64> m{};
    const auto size = encodeOscFloat(m, "/copy", 0.5f);
    REQUIRE(from.writeOsc(3, std::span(m).first(size)));
    REQUIRE(from.write(trigger(1)));
    for (const event& e : from.events()) CHECK(to.copyFrom(from, e));
    REQUIRE(to.events().size() == 2);
    const event& copied = to.events()[1];
    CHECK(copied.type == eventType::osc);
    CHECK(copied.sampleOffset == 3);
    const auto data = to.data(oscData(copied));
    CHECK(oscAddressIs(data, "/copy"));
    CHECK(oscFirstNumber(data) == 0.5f);
    from.beginBlock();  // the copy owns its bytes
    CHECK(oscAddressIs(to.data(oscData(copied)), "/copy"));
    // A full destination pool drops the copy and counts it.
    ePipe small(4, 8, overflows);
    CHECK_FALSE(small.copyFrom(to, copied));
    CHECK(*overflows == 1);
}

TEST_CASE("an event pipe outlives its table") {
    std::shared_ptr<pipe> kept;
    {
        pipeTable t({.audio = 1, .control = 1, .event = 1, .eventsPerPipe = 1, .poolBytes = 16},
                    {.sampleRate = 48000.0, .blockSize = 16});
        kept = t.find({.kind = metadata::pipeKind::event, .index = 0});
    }
    auto& p = static_cast<ePipe&>(*kept);
    CHECK(p.write(trigger(0)));
    CHECK_FALSE(p.write(trigger(0)));  // counts an overflow in the counter it shares
}

TEST_CASE("reconfiguring an audio pipe clears stale channels") {
    const std::vector<float> ones(4, 1.0f);
    aPipe p(4);
    p.configure(2, true);
    p.beginBlock(4);
    p.add(1, ones);
    p.endBlock();
    p.configure(1, true);  // channel 1 is inactive for two blocks
    for (int k = 0; k < 2; ++k) {
        p.beginBlock(4);
        p.endBlock();
    }
    p.configure(2, true);
    p.beginBlock(4);
    CHECK(p.read(1)[0] == 0.0f);
}

TEST_CASE("audio pipes ignore channels they do not have") {
    const std::vector<float> ones(4, 1.0f);
    aPipe p(4);
    p.configure(1, false);
    p.beginBlock(4);
    p.add(-1, ones);
    p.add(metadata::maxChannels, ones);
    p.addRamp(metadata::maxChannels + 3, ones, 0.0f, 1.0f);
    CHECK(p.read(0)[0] == 0.0f);
    CHECK(p.read(metadata::maxChannels).empty());
}

TEST_CASE("a control pipe that becomes a feedback pipe keeps its value") {
    cPipe c;
    CHECK(c.value() == 0.0f);
    c.configure(1, true);
    c.set(0, 5.0f);
    c.endBlock();
    c.configure(1, false);
    c.set(0, 7.0f);
    c.configure(1, true);
    c.endBlock();  // nothing written this block: the value holds
    CHECK(c.value() == 7.0f);
}

TEST_CASE("control channels activated by a reconfiguration hold channel 0") {
    cPipe c;
    c.set(0, 3.0f);
    c.configure(2, false);
    CHECK(c.value(1) == 3.0f);
}

TEST_CASE("the denormal guard flushes denormals to zero and restores the mode") {
    volatile float tiny = 1e-38f;
    const auto before = floatControl();
    {
        const denormalGuard guard;
        CHECK(floatMode() == nodeFloatControl);  // the whole mode, set
        volatile float product = tiny * 1e-3f;
        CHECK(product == 0.0f);
    }
    CHECK(floatControl() == before);
    volatile float product = tiny * 1e-3f;
    CHECK(product != 0.0f);
}

TEST_CASE("an audio pipe exists from the first make, which a second make keeps") {
    const prepareInfo info{.sampleRate = 48000.0, .blockSize = 16};
    pipeTable t({.audio = 2, .control = 1, .event = 0}, info);
    const metadata::pipeRef a{.kind = metadata::pipeKind::audio, .index = 1};
    CHECK(t.contains(a));
    CHECK(t.find(a) == nullptr);
    t.make(a);
    const auto made = t.find(a);
    REQUIRE(made != nullptr);
    CHECK(made->kind() == metadata::pipeKind::audio);
    t.make(a);
    CHECK(t.find(a) == made);
    CHECK(t.find({.kind = metadata::pipeKind::audio, .index = 0}) == nullptr);
    t.make({.kind = metadata::pipeKind::audio, .index = 2});  // beyond the capacity: nothing
    CHECK(t.find({.kind = metadata::pipeKind::audio, .index = 2}) == nullptr);
}

TEST_CASE("each pipe reports its kind, and its feedback flag once configured") {
    prepareInfo info{.sampleRate = 48000.0, .blockSize = 16};
    pipeTable t({.audio = 1, .control = 1, .event = 1}, info);
    t.make({.kind = metadata::pipeKind::audio, .index = 0});
    CHECK(t.audio(0).kind() == metadata::pipeKind::audio);
    CHECK(t.control(0).kind() == metadata::pipeKind::control);
    CHECK(t.events(0).kind() == metadata::pipeKind::event);
    CHECK_FALSE(t.audio(0).feedback());
    t.audio(0).configure(2, true);
    CHECK(t.audio(0).feedback());
    CHECK(t.audio(0).channels() == 2);
}
