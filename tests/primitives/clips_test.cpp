// Clips (canvas.md §5): the MIDI file reader, clip banks, and clipPlayer's
// changes on the grid.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/clip.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/event.h>

#include "support/graph.h"
#include "support/unwrap.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace support;
using namespace primitives;

namespace {

using bytes = std::vector<std::uint8_t>;

// A variable-length quantity.
bytes quantity(std::uint32_t v) {
    bytes out{static_cast<std::uint8_t>(v & 0x7FU)};
    while ((v >>= 7U) != 0) out.insert(out.begin(), static_cast<std::uint8_t>(0x80U | (v & 0x7FU)));
    return out;
}

bytes big(std::uint32_t v, int n) {
    bytes out;
    for (int i = n - 1; i >= 0; --i)
        out.push_back(static_cast<std::uint8_t>((v >> (8U * static_cast<unsigned>(i))) & 0xFFU));
    return out;
}

void append(bytes& to, const bytes& from) { to.insert(to.end(), from.begin(), from.end()); }

// A chunk of @p tag holding @p body.
bytes chunk(const std::string& tag, const bytes& body) {
    bytes out(tag.begin(), tag.end());
    append(out, big(static_cast<std::uint32_t>(body.size()), 4));
    append(out, body);
    return out;
}

// A MIDI file of @p format, @p division ticks a quarter note, and @p tracks.
bytes midiFile(const std::vector<bytes>& tracks, int format = 1, std::uint32_t division = 96) {
    bytes header = big(static_cast<std::uint32_t>(format), 2);
    append(header, big(static_cast<std::uint32_t>(tracks.size()), 2));
    append(header, big(division, 2));
    bytes out = chunk("MThd", header);
    for (const auto& t : tracks) append(out, chunk("MTrk", t));
    return out;
}

// A track's events: each a delta in ticks and its bytes; an end of track
// follows.
bytes track(const std::vector<std::pair<std::uint32_t, bytes>>& events, std::uint32_t endDelta = 0) {
    bytes out;
    for (const auto& [delta, e] : events) {
        append(out, quantity(delta));
        append(out, e);
    }
    append(out, quantity(endDelta));
    append(out, {0xFF, 0x2F, 0x00});
    return out;
}

clip read(const bytes& b) {
    std::string error;
    auto c = readMidiFile(b, error);
    INFO(error);
    return support::unwrap(c);
}

std::string refusal(const bytes& b) {
    std::string error;
    CHECK_FALSE(readMidiFile(b, error));
    return error;
}

std::unique_ptr<const clip> notesAt(std::vector<clipNote> notes, double length) {
    return std::make_unique<const clip>(clip{.notes = std::move(notes), .length = length});
}

clipNote note(double start, double end, int n) {
    return {.start = start, .end = end, .channel = 0, .note = static_cast<std::uint8_t>(n), .velocity = 100};
}

struct played {
    std::int64_t at = 0;
    eventType type = eventType::noteOn;
    int note = 0;
    friend bool operator==(const played&, const played&) = default;
};

// A clipPlayer of bank @p bank on a grid of @p grid beats, its slot chosen by
// control pipe 1, its notes on event pipe 0, the slot playing on control
// pipe 2 and the slot queued on 3; at 120 BPM a beat is 24000 samples.
struct player {
    explicit player(double grid = 4.0, const std::string& bank = "bass") {
        graph g;
        g.pipe(events(0)).pipe(control(1)).pipe(control(2)).pipe(control(3));
        auto& p = g.node("clips", "clipPlayer");
        p.params = {{"bank", bank}, {"grid", grid}, {"select", control(1)}};
        p.outputs["out"] = {events(0)};
        p.outputs["playing"] = {control(2)};
        p.outputs["queued"] = {control(3)};
        spec = g.spec;
    }

    // Plays the graph, then the clips offered so far start with its first block.
    void start() { r.play(spec); }

    void select(int slot) { REQUIRE(r.engine.setControl(1, static_cast<float>(slot))); }

    // Runs to sample @p until, block by block, recording the notes played.
    void runTo(std::int64_t until) {
        while (now < until) {
            r.run(64);
            for (const auto& e : r.engine.pipes().events(0).events())
                notes.push_back({.at = now + e.sampleOffset, .type = e.type, .note = data1(e)});
            now += 64;
        }
    }

    float playing() const { return r.engine.control(2); }
    float queued() const { return r.engine.control(3); }

    renderer r{1, 48000.0, 64};
    std::int64_t now = 0;
    std::vector<played> notes;
    metadata::graphSpec spec;
};

constexpr std::int64_t beat = 24000;

}  // namespace

TEST_CASE("a MIDI file reads as notes in beats, its tracks merged", "[clips]") {
    // Track 1: C4 from beat 0 for half a beat, running status, and E4 ended
    // by a note-on of velocity 0. Track 2: G4 from beat 1, never ended.
    const auto c = read(
        midiFile({track({{0, {0x90, 60, 100}}, {48, {0x80, 60, 0}}, {48, {0x90, 64, 90}}, {96, {64, 0}}}, 96),
                  track({{96, {0x91, 67, 80}}}, 192)}));
    REQUIRE(c.notes.size() == 3);
    CHECK(c.notes[0] == clipNote{.start = 0.0, .end = 0.5, .channel = 0, .note = 60, .velocity = 100});
    CHECK(c.notes[1] == clipNote{.start = 1.0, .end = 2.0, .channel = 0, .note = 64, .velocity = 90});
    // A note held at the end of its track ends there.
    CHECK(c.notes[2] == clipNote{.start = 1.0, .end = 3.0, .channel = 1, .note = 67, .velocity = 80});
    // The length runs to the end of the last track.
    CHECK(c.length == Approx(3.0));
    // Meta and system exclusive events are skipped.
    const auto withMeta = read(midiFile({track({{0, {0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}},
                                                {0, {0xF0, 0x02, 0x7E, 0xF7}},
                                                {0, {0x90, 60, 1}},
                                                {24, {0x80, 60, 0}}})},
                                        0));
    REQUIRE(withMeta.notes.size() == 1);
    CHECK(withMeta.notes[0].end == Approx(0.25));
}

TEST_CASE("files that are not MIDI files of format 0 or 1 in ticks are refused", "[clips]") {
    CHECK_THAT(refusal({'R', 'I', 'F', 'F'}), ContainsSubstring("not a Standard MIDI File"));
    CHECK_THAT(refusal(midiFile({}, 2)), ContainsSubstring("format 2"));
    CHECK_THAT(refusal(midiFile({}, 1, 0xE728)), ContainsSubstring("SMPTE"));
    auto cut = midiFile({track({{0, {0x90, 60, 100}}})});
    cut.resize(cut.size() - 6);
    CHECK_FALSE(refusal(cut).empty());
    CHECK_THAT(refusal(midiFile({track({{0, {60, 100}}})})), ContainsSubstring("status"));
    CHECK_THAT(refusal(midiFile({})), ContainsSubstring("without tracks"));
    // A header whose length runs past the file.
    auto header = midiFile({track({{0, {0x90, 60, 100}}})});
    header[7] = 0xF0;
    CHECK_THAT(refusal(header), ContainsSubstring("header longer than the file"));
}

TEST_CASE("a MIDI file's chunks of other kinds do not count as tracks", "[clips]") {
    // A header of one track, a vendor's chunk before it.
    bytes file = midiFile({}, 0);
    file[11] = 1;  // the header's track count
    append(file, chunk("XFIH", {1, 2, 3}));
    append(file, chunk("MTrk", track({{0, {0x90, 60, 100}}, {48, {0x80, 60, 0}}})));
    const auto c = read(file);
    REQUIRE(c.notes.size() == 1);
    CHECK(c.notes[0].note == 60);
}

TEST_CASE("a MIDI file's running status carries past meta events", "[clips]") {
    const auto c =
        read(midiFile({track({{0, {0x90, 60, 100}}, {0, {0xFF, 0x01, 0x01, 'x'}}, {48, {60, 0}}})}));
    REQUIRE(c.notes.size() == 1);
    CHECK(c.notes[0].end == Approx(0.5));
}

TEST_CASE("a MIDI file of more notes than a clip holds is refused, held notes counted", "[clips]") {
    // Note-ons never ended: each would end at the track's end.
    std::vector<std::pair<std::uint32_t, bytes>> ons;
    ons.reserve(maxClipNotes + 10);
    for (std::size_t i = 0; i < maxClipNotes + 10; ++i) ons.push_back({1, {0x90, 60, 100}});
    CHECK_THAT(refusal(midiFile({track(ons)})), ContainsSubstring("more than"));
}

TEST_CASE("a clip bank hands clips to its node and takes the old ones back", "[clips]") {
    clipBank bank;
    CHECK_FALSE(bank.at(0));
    REQUIRE(bank.offer(0, notesAt({note(0, 1, 60)}, 4)));
    CHECK_FALSE(bank.at(0));  // not taken yet
    bank.update();
    CHECK(support::unwrap(bank.at(0)).get().notes[0].note == 60);
    REQUIRE(bank.offer(0, nullptr));
    bank.update();
    CHECK_FALSE(bank.at(0));
    CHECK_FALSE(bank.offer(maxClipSlots, nullptr));
    // At most queueSize changes wait to be taken back.
    for (std::size_t i = 2; i < clipBank::queueSize; ++i) REQUIRE(bank.offer(1, nullptr));
    CHECK_FALSE(bank.offer(1, nullptr));
    // Once the node takes them and the control thread frees what it gave
    // back, there is room again.
    bank.update();
    bank.collect();
    CHECK(bank.offer(1, nullptr));
    // The engine keeps one bank a name.
    clipBanks banks;
    CHECK(banks.bank("bass") == banks.bank("bass"));
    CHECK(banks.bank("bass") != banks.bank("pad"));
    CHECK(banks.names() == std::vector<std::string>{"bass", "pad"});
}

TEST_CASE("clipPlayer loops its clip at whole bars from the grid line it started on", "[clips]") {
    player p;
    REQUIRE(p.r.engine.clips().bank("bass")->offer(0, notesAt({note(0, 0.5, 60), note(2, 2.5, 62)}, 3)));
    p.select(0);
    p.start();
    p.runTo(5 * beat);
    // A clip of three beats loops at a bar of four.
    const std::vector<played> expected{
        {.at = 0, .type = eventType::noteOn, .note = 60},
        {.at = beat / 2, .type = eventType::noteOff, .note = 60},
        {.at = 2 * beat, .type = eventType::noteOn, .note = 62},
        {.at = (5 * beat) / 2, .type = eventType::noteOff, .note = 62},
        {.at = 4 * beat, .type = eventType::noteOn, .note = 60},
        {.at = (9 * beat) / 2, .type = eventType::noteOff, .note = 60},
    };
    REQUIRE(p.notes.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO(i);
        CHECK(p.notes[i].type == expected[i].type);
        CHECK(p.notes[i].note == expected[i].note);
        CHECK(std::abs(p.notes[i].at - expected[i].at) <= 1);
    }
    CHECK(p.playing() == 0.0f);
    CHECK(p.queued() == 0.0f);
}

TEST_CASE("clipPlayer changes clip only on a grid line, ending the notes it holds there", "[clips]") {
    player p(4.0);
    const auto bank = p.r.engine.clips().bank("bass");
    REQUIRE(bank->offer(0, notesAt({note(0, 8, 60)}, 8)));
    REQUIRE(bank->offer(1, notesAt({note(0, 1, 72)}, 4)));
    p.select(0);
    p.start();
    p.runTo(beat);
    p.select(1);  // a beat in: the change waits for beat 4
    p.runTo(3 * beat);
    CHECK(p.playing() == 0.0f);
    CHECK(p.queued() == 1.0f);
    p.runTo((5 * beat) + 64);
    CHECK(p.playing() == 1.0f);
    CHECK(p.queued() == 1.0f);
    REQUIRE(p.notes.size() == 4);
    CHECK(p.notes[0] == played{.at = 0, .type = eventType::noteOn, .note = 60});
    // The held note of the first clip ends on the grid line, where the
    // second clip starts.
    CHECK(p.notes[1].type == eventType::noteOff);
    CHECK(p.notes[1].note == 60);
    CHECK(std::abs(p.notes[1].at - (4 * beat)) <= 1);
    CHECK(p.notes[2].type == eventType::noteOn);
    CHECK(p.notes[2].note == 72);
    CHECK(p.notes[2].at == p.notes[1].at);
    CHECK(p.notes[3].type == eventType::noteOff);
    // A selection of no slot, or of an empty one, stops on the next line.
    p.select(5);
    CHECK(p.queued() == 1.0f);
    p.runTo(6 * beat);
    CHECK(p.queued() == -1.0f);
    CHECK(p.playing() == 1.0f);
    p.runTo(9 * beat);
    CHECK(p.playing() == -1.0f);
}

TEST_CASE("clipPlayer plays a clip a bank takes while it plays, from where the old one was", "[clips]") {
    player p(4.0);
    const auto bank = p.r.engine.clips().bank("bass");
    REQUIRE(bank->offer(0, notesAt({note(0, 0.5, 60), note(2, 2.5, 62)}, 4)));
    p.select(0);
    p.start();
    p.runTo(beat);
    REQUIRE(bank->offer(0, notesAt({note(0, 0.5, 48), note(2, 2.5, 50)}, 4)));
    p.runTo(3 * beat);
    p.r.engine.collect();
    REQUIRE(p.notes.size() == 4);
    CHECK(p.notes[2].note == 50);  // the new clip's note at beat 2
    CHECK(std::abs(p.notes[2].at - (2 * beat)) <= 1);
}

TEST_CASE("a clipPlayer that adopts another's state keeps its clip, place, and held notes", "[clips]") {
    player p(4.0);
    REQUIRE(p.r.engine.clips().bank("bass")->offer(0, notesAt({note(0, 2, 60)}, 4)));
    p.select(0);
    p.start();
    p.runTo(beat);
    // The same graph again: the node adopts its state, and the note it
    // holds still ends at beat 2.
    p.r.play(p.spec, 2);
    p.runTo(3 * beat);
    REQUIRE(p.notes.size() == 2);
    CHECK(p.notes[1].type == eventType::noteOff);
    CHECK(std::abs(p.notes[1].at - (2 * beat)) <= 1);
    CHECK(p.playing() == 0.0f);
}

TEST_CASE("a clipPlayer of another bank takes over none of the old one's state", "[clips]") {
    player p(4.0);
    REQUIRE(p.r.engine.clips().bank("bass")->offer(0, notesAt({note(0, 4, 60)}, 4)));
    REQUIRE(p.r.engine.clips().bank("pad")->offer(0, notesAt({note(1, 2, 72)}, 4)));
    p.select(0);
    p.start();
    p.runTo(beat / 2);
    // The same node of bank "pad": the bass's note ends with its player,
    // and the pad's player starts its own clip.
    p.r.play(player(4.0, "pad").spec, 2);
    p.runTo(3 * beat);
    REQUIRE(p.notes.size() == 4);
    // It ends in the old player's last block, the first after the swap.
    CHECK(p.notes[1].type == eventType::noteOff);
    CHECK(p.notes[1].note == 60);
    CHECK(p.notes[1].at < beat / 2 + 128);
    CHECK(p.notes[2].type == eventType::noteOn);
    CHECK(p.notes[2].note == 72);
    CHECK(std::abs(p.notes[2].at - beat) <= 1);
}

TEST_CASE("a clipPlayer added while the clock runs starts at once, in time with the grid", "[clips]") {
    player p(4.0);
    graph empty;
    empty.pipe(events(0)).pipe(control(1)).pipe(control(2)).pipe(control(3));
    p.r.play(empty.spec);
    p.runTo((5 * beat) + (beat / 4));
    p.select(0);
    p.r.play(p.spec, 2);
    p.runTo(7 * beat);
    // Its first clip arrives after it starts; it plays from where it would
    // be had it started on the line at beat 4.
    REQUIRE(p.r.engine.clips().bank("bass")->offer(
        0, notesAt({note(0, 0.5, 60), note(2.5, 3, 62), note(3.5, 4, 64)}, 4)));
    p.runTo(9 * beat);
    REQUIRE(p.notes.size() == 4);
    CHECK(p.notes[0] == played{.at = (15 * beat) / 2, .type = eventType::noteOn, .note = 64});
    CHECK(p.notes[1] == played{.at = 8 * beat, .type = eventType::noteOn, .note = 60});
    CHECK(p.playing() == 0.0f);
}

TEST_CASE("a clipPlayer's choice past its slots is no slot", "[clips]") {
    player p(4.0);
    REQUIRE(p.r.engine.clips().bank("bass")->offer(0, notesAt({note(0, 1, 60)}, 4)));
    REQUIRE(p.r.engine.setControl(1, 4294967296.0f));  // 2^32, which wrapped to slot 0
    p.start();
    p.runTo(5 * beat);
    CHECK(p.notes.empty());
    CHECK(p.playing() == -1.0f);
}

TEST_CASE("a playing clip keeps the bars it started in when the meter changes", "[clips]") {
    player p(4.0);
    REQUIRE(p.r.engine.clips().bank("bass")->offer(0, notesAt({note(0, 0.5, 60)}, 3)));
    p.select(0);
    p.start();
    p.runTo(beat);
    // The same piece in 3/4: the clip of three beats loops at the bar of
    // four it started in, not at a bar of three.
    auto threeFour = p.spec;
    threeFour.meter = 3;
    p.r.play(threeFour, 2);
    p.runTo(5 * beat);
    std::vector<std::int64_t> ons;
    for (const auto& n : p.notes) {
        if (n.type == eventType::noteOn) ons.push_back(n.at);
    }
    REQUIRE(ons.size() == 2);
    CHECK(std::abs(ons[1] - (4 * beat)) <= 1);
}

TEST_CASE("a block of several loops plays every one of its notes", "[clips]") {
    // A loop of one beat at 4/4's floor of a bar, in blocks of 16 beats.
    renderer r{1, 48000.0, 4096};
    graph g;
    g.spec.meter = 1;
    g.spec.tempo = 7000.0;  // 7000 BPM: a beat is about 411 samples
    g.pipe(events(0)).pipe(control(1)).pipe(control(2)).pipe(control(3));
    auto& n = g.node("clips", "clipPlayer");
    n.params = {{"bank", std::string("bass")}, {"grid", 1.0}, {"select", control(1)}};
    n.outputs["out"] = {events(0)};
    n.outputs["playing"] = {control(2)};
    n.outputs["queued"] = {control(3)};
    REQUIRE(r.engine.clips().bank("bass")->offer(0, notesAt({note(0, 0.25, 60)}, 1)));
    REQUIRE(r.engine.setControl(1, 0.0f));
    r.play(g.spec);
    int ons = 0;
    for (int b = 0; b < 4; ++b) {
        r.run(4096);
        for (const auto& e : r.engine.pipes().events(0).events()) ons += e.type == eventType::noteOn ? 1 : 0;
    }
    // Four blocks of about ten beats each: a note on every beat.
    CHECK(ons >= 39);
}

TEST_CASE("a clipPlayer that starts at once on a bar line plays the note on it", "[clips]") {
    // At 120 BPM in blocks of 256, every bar line falls on a block's start,
    // where the beat clock, a running sum, often reaches it a hair late.
    for (int bars = 1; bars <= 8; ++bars) {
        INFO(bars);
        renderer r{1, 48000.0, 256};
        graph empty;
        empty.pipe(events(0)).pipe(control(1)).pipe(control(2)).pipe(control(3));
        r.play(empty.spec);
        for (int b = 0; b < bars * 375; ++b) r.run(256);
        REQUIRE(r.engine.clips().bank("bass")->offer(0, notesAt({note(0, 0.5, 60)}, 4)));
        REQUIRE(r.engine.setControl(1, 0.0f));
        r.play(player(4.0).spec, 2);
        r.run(256);
        const auto events = r.engine.pipes().events(0).events();
        REQUIRE(events.size() == 1);
        CHECK(events[0].type == eventType::noteOn);
        CHECK(events[0].sampleOffset == 0);
    }
}

TEST_CASE("a clipPlayer waiting for its first clip still starts at once after a swap", "[clips]") {
    player p(4.0);
    p.select(0);
    p.start();
    p.runTo(beat);
    p.r.play(p.spec, 2);  // adopts the waiting player
    p.runTo((5 * beat) + (beat / 4));
    REQUIRE(p.r.engine.clips().bank("bass")->offer(0, notesAt({note(0, 0.5, 60), note(2, 2.5, 62)}, 4)));
    p.runTo(7 * beat);
    REQUIRE_FALSE(p.notes.empty());
    CHECK(p.notes[0] == played{.at = 6 * beat, .type = eventType::noteOn, .note = 62});
}
