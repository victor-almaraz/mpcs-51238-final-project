// serialIn and serialOut against frames delivered and drained through the
// hub (io.md §4.7, §4.8, §11.2).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/event.h>
#include <espresso/primitives/io.h>

#include "support/graph.h"

using Catch::Approx;
using namespace support;
using namespace primitives;

namespace {

constexpr double rate = 48000.0;
constexpr int block = 64;
constexpr std::string_view device = "/dev/ttyACM*";

// A frame of @p values received at sample @p at of the block after the
// one rendered last, which renderer::run() has just advanced past.
dataFrame frameOf(std::initializer_list<float> values, double at = 0.5) {
    dataFrame f;
    for (const float v : values) f.values[f.count++] = v;
    f.time = at / rate;
    return f;
}

// serialIn on device, into 3-channel control 1 and events 0.
graph reader() {
    graph g;
    g.pipe(control(1), 3).pipe(events(0));
    auto& in = g.node("serial", "serialIn");
    in.params["device"] = std::string(device);
    in.outputs["value"] = {control(1)};
    in.outputs["out"] = {events(0)};
    return g;
}

std::vector<float> channels(renderer& r, int count) {
    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(count));
    for (int ch = 0; ch < count; ++ch) out.push_back(r.engine.pipes().control(1).value(ch));
    return out;
}

// The frames queued for device, drained.
std::vector<dataFrame> sent(renderer& r) {
    std::vector<dataFrame> out;
    r.engine.io().drainSerialOut([&](const serialPort& port, const dataFrame& f) {
        CHECK(port.device() == device);
        out.push_back(f);
    });
    return out;
}

std::vector<float> valuesOf(const dataFrame& f) {
    const auto used = std::span(f.values).first(f.count);
    return {used.begin(), used.end()};
}

}  // namespace

TEST_CASE("serialIn writes a frame's values to their channels and an event per line (io.md 4.7)") {
    renderer r(1, rate, block);
    r.play(reader().spec);
    r.run(block);
    r.engine.io().deliverSerial(device, frameOf({512.0f, 0.25f, 1.0f}, 10.5));
    r.run(block);
    CHECK(channels(r, 3) == std::vector<float>{512.0f, 0.25f, 1.0f});
    const auto events = r.engine.pipes().events(0).events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == eventType::value);
    CHECK(events[0].sampleOffset == 10);  // where it arrived in the previous block period
    CHECK(number(events[0]) == 512.0f);

    // A shorter line leaves the channels it lacks alone; a longer one fills
    // only the pipe's channels.
    r.engine.io().deliverSerial(device, frameOf({7.0f}));
    r.run(block);
    CHECK(channels(r, 3) == std::vector<float>{7.0f, 0.25f, 1.0f});
    r.engine.io().deliverSerial(device, frameOf({1.0f, 2.0f, 3.0f, 4.0f, 5.0f}));
    r.run(block);
    CHECK(channels(r, 3) == std::vector<float>{1.0f, 2.0f, 3.0f});
}

TEST_CASE("serialIn reads only its own device pattern and drops non-finite lines") {
    renderer r(1, rate, block);
    r.play(reader().spec);
    r.run(block);
    r.engine.io().deliverSerial("/dev/ttyUSB0", frameOf({9.0f}));
    const auto drops = r.engine.io().drops();
    r.engine.io().deliverSerial(device, frameOf({3.0f, std::nanf("")}));
    r.engine.io().deliverSerial(device, frameOf({std::numeric_limits<float>::infinity()}));
    r.run(block);
    CHECK(channels(r, 3) == std::vector<float>{0.0f, 0.0f, 0.0f});
    CHECK(r.engine.pipes().events(0).events().empty());
    CHECK(r.engine.io().drops() == drops + 2);
}

TEST_CASE("an adopting serialIn keeps the frames queued at a swap (io.md 3.5)") {
    const auto g = reader();
    renderer r(1, rate, block);
    r.play(g.spec, 1);
    r.run(block);
    r.engine.io().deliverSerial(device, frameOf({5.0f}));
    r.play(g.spec, 2);  // queued in plan 1's port, read by plan 2's node
    r.run(block);
    CHECK(channels(r, 1) == std::vector<float>{5.0f});
    r.engine.io().deliverSerial(device, frameOf({6.0f}));
    r.run(block);
    CHECK(channels(r, 1) == std::vector<float>{6.0f});
}

TEST_CASE("serialOut sends a line per event: its value channels or the event's number (io.md 4.8)") {
    auto g = reader();
    auto& withValue = g.node("echo", "serialOut");
    withValue.params["device"] = std::string(device);
    withValue.inputs["in"] = {events(0)};
    withValue.inputs["value"] = {control(1)};
    auto& plain = g.node("count", "serialOut");
    plain.params["device"] = std::string(device);
    plain.inputs["in"] = {events(0)};
    renderer r(1, rate, block);
    r.play(g.spec);
    r.run(block);
    r.engine.io().deliverSerial(device, frameOf({2.0f, 0.5f, -1.0f}, 20.5));
    r.run(block);
    auto frames = sent(r);
    REQUIRE(frames.size() == 2);
    // The ports drain in the order the nodes opened them.
    CHECK(valuesOf(frames[0]) == std::vector<float>{2.0f, 0.5f, -1.0f});
    CHECK(valuesOf(frames[1]) == std::vector<float>{2.0f});
    // The event's sample, 20 in the second block, on the device clock.
    CHECK(frames[0].time == Approx((block + 20.0) / rate));
}

TEST_CASE("serialOut sends nothing for a control value alone") {
    graph g;
    g.pipe(control(1)).pipe(events(0));
    auto& lfo = g.node("lfo", "lfo");
    lfo.params["rate"] = 10.0;
    lfo.outputs["out"] = {control(1)};
    auto& out = g.node("out", "serialOut");
    out.params["device"] = std::string(device);
    out.inputs["in"] = {events(0)};
    out.inputs["value"] = {control(1)};
    renderer r(1, rate, block);
    r.play(g.spec);
    r.run(block * 20);
    CHECK(sent(r).empty());
}
