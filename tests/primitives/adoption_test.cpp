// State adoption at a plan swap (primitives.md §9.2): a node of the next
// plan with the same ID and shape carries on from the node it replaces,
// for the primitives whose state outlives a block.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/io.h>

#include "support/graph.h"

using namespace support;
using namespace primitives;

namespace {

// Renders @p g for @p before samples, then, when @p swap, plays it again
// as plan 2 with no fade, then renders @p after samples; returns output 0.
std::vector<float> render(const metadata::graphSpec& g, int before, int after, bool swap) {
    renderer r(1);
    r.play(g, 1);
    r.run(before);
    if (swap) r.play(g, 2);
    r.run(after);
    return r.out();
}

// A trigger counter on control pipe 1: a sequencer stepping 1, 2, 3, ...
void countInto(graph& g, pipeRef triggers) {
    auto& count = g.node("count", "sequencer");
    std::vector<double> values(4000);
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = static_cast<double>(i + 1);
    count.params["values"] = std::move(values);
    count.inputs["in"] = {triggers};
    count.outputs["out"] = {control(1)};
}

}  // namespace

TEST_CASE("an adopted noise generator continues its random sequence") {
    graph g;
    g.pipe(audio(0));
    g.node("noise", "noise").outputs["out"] = {audio(0)};
    g.node("out", "audioOut").inputs["in"] = {audio(0)};
    CHECK(render(g.spec, 4800, 4800, true) == render(g.spec, 4800, 4800, false));
}

TEST_CASE("an adopted chance node continues its random choices") {
    const auto passed = [](bool swap) {
        graph g;
        g.spec.tempo = 6000.0;  // 100 beats a second
        g.pipe(events(0)).pipe(events(2)).pipe(control(1));
        auto& m = g.node("metro", "metro");
        m.params["every"] = 0.25;
        m.outputs["out"] = {events(0)};
        auto& c = g.node("chance", "chance");
        c.params["probability"] = 0.5;
        c.inputs["in"] = {events(0)};
        c.outputs["out"] = {events(2)};
        countInto(g, events(2));
        renderer r(1);
        r.play(g.spec, 1);
        r.run(24000);
        if (swap) r.play(g.spec, 2);
        r.run(24000);
        return r.engine.control(1);
    };
    const float continuous = passed(false);
    CHECK(continuous > 100.0f);  // about half of 400 triggers
    CHECK(passed(true) == continuous);
}

TEST_CASE("an adopted threshold remembers which side of its level the input was on") {
    const auto crossings = [](bool swap) {
        graph g;
        g.pipe(audio(0)).pipe(events(2)).pipe(control(1));
        auto& lfo = g.node("lfo", "lfo");
        lfo.params["rate"] = 10.0;
        lfo.outputs["out"] = {audio(0)};
        auto& t = g.node("threshold", "threshold");
        t.params["level"] = 0.0;
        t.params["direction"] = std::string("rising");
        t.inputs["in"] = {audio(0)};
        t.outputs["out"] = {events(2)};
        countInto(g, events(2));
        renderer r(1);
        r.play(g.spec, 1);
        r.run(24000);
        if (swap) r.play(g.spec, 2);
        r.run(24000);
        return r.engine.control(1);
    };
    const float continuous = crossings(false);
    CHECK(continuous >= 9.0f);  // ten cycles a second
    CHECK(crossings(true) == continuous);
}

TEST_CASE("an adopted pitchBend reader keeps the last bend") {
    graph g;
    g.pipe(events(0)).pipe(control(1));
    g.node("midi", "midiIn").outputs["out"] = {events(0)};
    auto& bend = g.node("bend", "pitchBend");
    bend.params["range"] = 2.0;
    bend.inputs["in"] = {events(0)};
    bend.outputs["out"] = {control(1)};
    renderer r(1);
    r.play(g.spec, 1);
    r.run(64);
    r.engine.io().deliverMidi("k", midi(0xe0, 0x7f, 0x7f));  // full bend up
    r.run(64);
    const float bent = r.engine.control(1);
    CHECK(bent > 1.9f);
    r.play(g.spec, 2);
    r.run(128);
    CHECK(r.engine.control(1) == bent);
}

TEST_CASE("an adopted voiceSplit routes a held note's note-off to its voice") {
    graph g;
    g.pipe(events(0)).pipe(events(1)).pipe(events(2)).pipe(control(1));
    g.node("midi", "midiIn").outputs["out"] = {events(0)};
    auto& split = g.node("split", "voiceSplit");
    split.inputs["in"] = {events(0)};
    split.outputs["out"] = {events(1), events(2)};
    auto& gate = g.node("gate", "gate");
    gate.inputs["in"] = {events(1)};
    gate.outputs["out"] = {control(1)};
    renderer r(1);
    r.play(g.spec, 1);
    r.run(64);
    r.engine.io().deliverMidi("k", midi(0x90, 60, 100));
    r.run(64);
    CHECK(r.engine.control(1) == 1.0f);
    r.play(g.spec, 2);
    r.run(64);
    // Still held: a replaced split, not adopted, would have ended it at the swap.
    CHECK(r.engine.control(1) == 1.0f);
    r.engine.io().deliverMidi("k", midi(0x80, 60, 0));
    r.run(64);
    CHECK(r.engine.control(1) == 0.0f);  // the split knew the note was on voice 0
}

TEST_CASE("an adopted triggerNotes ends the notes it started before the swap") {
    graph g;
    g.pipe(events(0)).pipe(events(2)).pipe(control(1));
    auto& m = g.node("metro", "metro");
    m.params["every"] = 64.0;  // one trigger, at the start
    m.outputs["out"] = {events(0)};
    auto& notes = g.node("notes", "triggerNotes");
    notes.params["length"] = 0.5;
    notes.inputs["in"] = {events(0)};
    notes.outputs["out"] = {events(2)};
    auto& gate = g.node("gate", "gate");
    gate.inputs["in"] = {events(2)};
    gate.outputs["out"] = {control(1)};
    renderer r(1);
    r.play(g.spec, 1);
    r.run(4800);
    CHECK(r.engine.control(1) == 1.0f);
    r.play(g.spec, 2);
    r.run(4800);
    // Still sounding at 0.2 s: a replaced node, not adopted, ends its notes at the swap.
    CHECK(r.engine.control(1) == 1.0f);
    r.run(24000);  // past the note's end at 0.5 s
    CHECK(r.engine.control(1) == 0.0f);
}

TEST_CASE("an adopted triangle oscillator continues its waveform") {
    graph g;
    g.pipe(audio(0));
    auto& osc = g.node("tri", "triangleOsc");
    osc.params["freq"] = 330.0;
    osc.outputs["out"] = {audio(0)};
    g.node("out", "audioOut").inputs["in"] = {audio(0)};
    CHECK(render(g.spec, 4800, 4800, true) == render(g.spec, 4800, 4800, false));
}
