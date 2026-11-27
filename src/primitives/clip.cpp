// Clips (canvas.md §5): the MIDI file reader, clip banks, and clipPlayer,
// which plays one bank's clips, changing clip only on a grid line of the
// beat clock.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/clip.h>
#include <espresso/primitives/event.h>
#include <espresso/primitives/pipe.h>
#include <espresso/primitives/primitive.h>
#include <espresso/primitives/realtime.h>

#include "node.h"

namespace primitives {

// --- readMidiFile -------------------------------------------------------------

namespace {

// Reads big-endian fields and variable-length quantities from a span,
// failing (ok() false) at its end rather than past it.
class byteReader {
public:
    explicit byteReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    bool ok() const noexcept { return ok_; }
    bool atEnd() const noexcept { return at_ >= bytes_.size(); }
    std::size_t left() const noexcept { return bytes_.size() - at_; }

    std::uint32_t byte() {
        if (atEnd()) return fail();
        return bytes_[at_++];
    }
    std::uint32_t peek() const noexcept { return atEnd() ? 0 : bytes_[at_]; }
    std::uint32_t big(int count) {
        std::uint32_t v = 0;
        for (int i = 0; i < count; ++i) v = (v << 8U) | byte();
        return v;
    }
    // At most four bytes, as the standard allows.
    std::uint32_t quantity() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t b = byte();
            v = (v << 7U) | (b & 0x7FU);
            if ((b & 0x80U) == 0) return v;
        }
        return fail();
    }
    std::span<const std::uint8_t> take(std::size_t n) {
        if (n > left()) {
            fail();
            return {};
        }
        const auto out = bytes_.subspan(at_, n);
        at_ += n;
        return out;
    }
    bool tag(std::string_view expected) {
        const auto got = take(expected.size());
        return ok_ &&
               std::ranges::equal(got, expected, {}, {}, [](char c) { return static_cast<std::uint8_t>(c); });
    }

private:
    std::uint32_t fail() noexcept {
        ok_ = false;
        at_ = bytes_.size();
        return 0;
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t at_ = 0;
    bool ok_ = true;
};

// Reads one track's notes into @p notes, in ticks; returns its end, in
// ticks, or nothing when malformed.
std::optional<std::uint64_t> readTrack(std::span<const std::uint8_t> bytes, std::vector<clipNote>& notes,
                                       std::string& error) {
    byteReader in(bytes);
    std::uint64_t tick = 0;
    std::uint32_t status = 0;  // the running status: the last channel event's
    // The ticks and velocities of the notes held, by channel and note, first
    // held first; a note-off ends the first.
    std::map<std::pair<int, int>, std::deque<std::pair<std::uint64_t, int>>> held;
    std::size_t holding = 0;
    const auto end = [&](int channel, int note, std::uint64_t at) {
        auto it = held.find({channel, note});
        if (it == held.end() || it->second.empty()) return;
        const auto [from, velocity] = it->second.front();
        it->second.pop_front();
        --holding;
        notes.push_back({.start = static_cast<double>(from),
                         .end = static_cast<double>(std::max(at, from + 1)),
                         .channel = static_cast<std::uint8_t>(channel),
                         .note = static_cast<std::uint8_t>(note),
                         .velocity = static_cast<std::uint8_t>(velocity)});
    };
    while (in.ok() && !in.atEnd()) {
        tick += in.quantity();
        // Meta and system exclusive events leave the running status as it
        // was: a file that runs a channel's status on past them reads.
        const std::uint32_t lead = in.peek();
        if (lead == 0xFF) {  // a meta event
            in.byte();
            const std::uint32_t type = in.byte();
            in.take(in.quantity());
            if (type == 0x2F) break;  // the end of the track
            continue;
        }
        if (lead == 0xF0 || lead == 0xF7) {  // system exclusive
            in.byte();
            in.take(in.quantity());
            continue;
        }
        if ((lead & 0x80U) != 0) status = in.byte();
        if (status < 0x80 || status >= 0xF0) {
            error = "a MIDI event without a status byte";
            return std::nullopt;
        }
        const std::uint32_t kind = status & 0xF0U;
        const int channel = static_cast<int>(status & 0x0FU);
        const std::uint32_t first = in.byte();
        const std::uint32_t second = (kind == 0xC0 || kind == 0xD0) ? 0 : in.byte();
        const int note = static_cast<int>(first & 0x7FU);
        if (kind == 0x90 && second > 0) {
            held[{channel, note}].emplace_back(tick, static_cast<int>(second & 0x7FU));
            ++holding;
        } else if (kind == 0x80 || kind == 0x90) {
            end(channel, note, tick);
        }
        // Each note held ends at the track's end at the latest: the notes
        // held count.
        if (notes.size() + holding > maxClipNotes) {
            error = "more than " + std::to_string(maxClipNotes) + " notes";
            return std::nullopt;
        }
    }
    if (!in.ok()) {
        error = "a track ends inside an event";
        return std::nullopt;
    }
    for (auto& [key, list] : held) {
        while (!list.empty()) end(key.first, key.second, tick);
    }
    return tick;
}

}  // namespace

std::optional<clip> readMidiFile(std::span<const std::uint8_t> bytes, std::string& error) {
    byteReader in(bytes);
    if (!in.tag("MThd")) {
        error = "not a Standard MIDI File";
        return std::nullopt;
    }
    const std::uint32_t headerLength = in.big(4);
    const std::uint32_t format = in.big(2);
    const std::uint32_t tracks = in.big(2);
    const std::uint32_t division = in.big(2);
    if (!in.ok() || headerLength < 6) {
        error = "a MIDI file header too short";
        return std::nullopt;
    }
    in.take(headerLength - 6);
    if (!in.ok()) {
        error = "a MIDI file header longer than the file";
        return std::nullopt;
    }
    if (format > 1) {
        error = "a MIDI file of format " + std::to_string(format) + "; formats 0 and 1 are read";
        return std::nullopt;
    }
    if ((division & 0x8000U) != 0 || division == 0) {
        error = "a MIDI file in SMPTE time; files in ticks a quarter note are read";
        return std::nullopt;
    }
    if (tracks == 0) {
        error = "a MIDI file without tracks";
        return std::nullopt;
    }
    std::vector<clipNote> notes;
    std::uint64_t last = 0;
    bool read = false;
    for (std::uint32_t t = 0; t < tracks && in.ok() && !in.atEnd(); ++t) {
        const bool track = in.tag("MTrk");
        const std::uint32_t length = in.big(4);
        const auto body = in.take(length);
        if (!in.ok()) {
            error = "a MIDI file cut short";
            return std::nullopt;
        }
        if (!track) continue;  // a chunk of another kind is skipped, as the standard asks
        const auto end = readTrack(body, notes, error);
        if (!end) return std::nullopt;
        last = std::max(last, *end);
        read = true;
    }
    if (!read) {
        error = "a MIDI file without tracks";
        return std::nullopt;
    }
    if (notes.size() > maxClipNotes) {
        error = "more than " + std::to_string(maxClipNotes) + " notes";
        return std::nullopt;
    }
    const double perBeat = division;
    clip out;
    for (auto& n : notes) {
        n.start /= perBeat;
        n.end /= perBeat;
        out.length = std::max(out.length, n.end);
    }
    out.length = std::max(out.length, static_cast<double>(last) / perBeat);
    std::ranges::stable_sort(notes, {}, &clipNote::start);
    out.notes = std::move(notes);
    return out;
}

// --- clipBank, clipBanks --------------------------------------------------------

bool clipBank::offer(int slot, std::unique_ptr<const clip> c) {
    if (slot < 0 || slot >= maxClipSlots || inFlight_ >= queueSize) return false;
    if (!incoming_.push(change{.slot = slot, .item = std::move(c)})) return false;
    ++inFlight_;
    return true;
}

void clipBank::collect() {
    std::unique_ptr<const clip> given;
    while (outgoing_.pop(given)) {
        given.reset();
        --inFlight_;
    }
}

void clipBank::update() noexcept {
    while (incoming_.pop(taken_)) {
        // Swapped, not assigned: the slot's clip leaves in taken_, which
        // gives it back to the control thread, so nothing is freed here.
        std::swap(slots_[static_cast<std::size_t>(taken_.slot)], taken_.item);
        // Never full: it holds at most one item for each change in flight,
        // so taken_ is left empty for the next pop.
        [[maybe_unused]] const bool given = outgoing_.push(std::move(taken_.item));
    }
}

metadata::optionalRef<clip> clipBank::at(int slot) const noexcept {
    if (slot < 0 || slot >= maxClipSlots) return std::nullopt;
    const auto& c = slots_[static_cast<std::size_t>(slot)];
    if (!c) return std::nullopt;
    return std::cref(*c);
}

std::shared_ptr<clipBank> clipBanks::bank(const std::string& name) {
    auto& b = banks_[name];
    if (!b) b = std::make_shared<clipBank>();
    return b;
}

std::vector<std::string> clipBanks::names() const {
    std::vector<std::string> out;
    out.reserve(banks_.size());
    for (const auto& [name, b] : banks_) out.push_back(name);
    return out;
}

void clipBanks::collect() {
    for (auto& [name, b] : banks_) b->collect();
}

// --- clipPlayer -------------------------------------------------------------------

namespace {

// Plays bank `bank`'s clip in slot `select`, from 0 (none below 0 or past
// `slots`), as notes on `out`. A change of slot waits for the next grid
// line, a multiple of `grid` beats on the beat clock, the clip playing
// until then; a clip loops at its length rounded up to whole bars, from
// the grid line it started on. Notes held when a clip changes or stops
// end there; a player new to the plan starts at once (start()).
// `playing` is the slot playing, or −1; `queued` the slot it changes to at
// the next grid line, or `playing` when none waits.
class clipPlayer final : public nodeBase {
public:
    explicit clipPlayer(nodeContext& c)
        : nodeBase(c),
          bank_(c.clips(c.text("bank"))),
          slots_(static_cast<int>(c.number("slots"))),
          grid_(c.number("grid")),
          select_(c.param("select")),
          out_(c.eventOut("out")),
          playingOut_(c.controlOut("playing")),
          queuedOut_(c.controlOut("queued")) {}

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        const auto& o = static_cast<const clipPlayer&>(outgoing);
        playing_ = o.playing_;
        launch_ = o.launch_;
        last_ = o.last_;
        held_ = o.held_;
        count_ = o.count_;
        meter_ = o.meter_;
        // One still waiting to start goes on waiting to start at once.
        fresh_ = o.fresh_;
        first_ = o.first_;
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const double perSample = ctx.beatsPerSample();
        if (perSample <= 0.0) return;
        const auto n = ctx.numSamples;
        // The block's beats, from the last block's end when the clock ran
        // on from it, so a note at a block's edge plays once.
        const double end = ctx.beat + (perSample * n);
        double from = std::isfinite(last_) && std::abs(ctx.beat - last_) < 2.0 * perSample ? last_ : ctx.beat;
        last_ = end;
        const auto offset = [&](double beat) {
            const double at = std::floor((beat - ctx.beat) / perSample);
            return static_cast<std::uint32_t>(std::clamp(at, 0.0, n - 1.0));
        };
        if (retiring()) {
            // A retiring node takes no clips (its replacement may be taking
            // them) and starts no notes; it ends those it holds.
            endDue(end, offset);
            if (finalBlock()) endAll(end, offset);
            return;
        }
        bank_->update();
        const int wanted = selected();
        // A start at once takes the notes on its grid line too: the beat
        // clock, a running sum, reaches a line on a block's start a hair late.
        if (fresh_ && start(from, wanted, ctx.meter)) from = std::max(launch_, from - lineTolerance);
        if (wanted != playing_) {
            const double line = std::ceil((from / grid_) - lineTolerance) * grid_;
            if (line < end) {
                play(from, line, offset);
                endDue(line, offset);
                endAll(line, offset);
                playing_ = wanted;
                launch_ = line;
                meter_ = std::max(1, ctx.meter);
                play(line, end, offset);
            } else {
                play(from, end, offset);
            }
        } else {
            play(from, end, offset);
        }
        endDue(end, offset);
        emitAll(playingOut_, static_cast<float>(playing_));
        emitAll(queuedOut_, static_cast<float>(selected()));
    }

private:
    struct heldNote {
        double end = 0.0;  // beat of its note-off
        std::uint8_t channel = 0;
        std::uint8_t note = 0;
    };

    // The slot chosen, whether or not it holds a clip; −1 for none.
    int chosen() const noexcept {
        const float v = select_.value();
        // Tested before rounding, so a huge value is no slot rather than a
        // wrapped one.
        if (!(v >= 0.0f) || v >= static_cast<float>(slots_)) return -1;
        const auto slot = static_cast<int>(std::lround(v));
        return slot < slots_ ? slot : -1;
    }

    // The slot chosen, when it holds a clip; −1 otherwise.
    int selected() const noexcept {
        const int slot = chosen();
        return slot >= 0 && bank_->at(slot) ? slot : -1;
    }

    // A player that took over no state starts the slot chosen when it
    // began at once, where the clip would be had it started on the last
    // grid line, so it keeps time with the players already playing. It
    // waits for that slot's clip to arrive; a choice of another slot, or
    // of none, is a change like any other.
    // Returns true when it starts.
    bool start(double from, int wanted, int meter) noexcept {
        if (first_ < -1) first_ = chosen();
        if (chosen() != first_ || first_ < 0) {
            fresh_ = false;
        } else if (wanted >= 0) {
            fresh_ = false;
            playing_ = wanted;
            launch_ = std::floor((from / grid_) + lineTolerance) * grid_;
            meter_ = std::max(1, meter);
            return true;
        }
        return false;
    }

    // The playing clip's notes that start in [from, to), each after the
    // notes due before it end.
    template <typename Offset>
    void play(double from, double to, const Offset& offset) noexcept {
        if (playing_ < 0 || to <= from) return;
        const auto c = bank_->at(playing_);
        if (!c) return;
        const clip& playingClip = c->get();
        // In bars of the meter it started in, so a change of meter keeps a
        // playing loop on its grid; the next clip takes the new one.
        const double bar = meter_;
        const double length = std::max(1.0, std::ceil((playingClip.length / bar) - 1e-9)) * bar;
        const auto first = static_cast<std::int64_t>(std::floor((from - launch_) / length));
        // The passes the block spans, a loop at least a beat long: a fixed
        // bound past any tempo and block the engine runs.
        const auto passes =
            static_cast<std::int64_t>(std::min(maxPasses, std::ceil((to - from) / length) + 1.0));
        for (std::int64_t k = first; k < first + passes; ++k) {
            const double start = launch_ + (static_cast<double>(k) * length);
            if (start >= to) break;
            const double lo = std::max(from, start) - start;
            const double hi = std::min(to, start + length) - start;
            auto it = std::ranges::lower_bound(playingClip.notes, lo, {}, &clipNote::start);
            for (; it != playingClip.notes.end() && it->start < hi; ++it) {
                const double at = start + it->start;
                endDue(at, offset);
                // A note sounding again ends first.
                for (std::size_t i = 0; i < count_; ++i) {
                    if (held_[i].note == it->note && held_[i].channel == it->channel) {
                        end(i, at, offset);
                        break;
                    }
                }
                if (count_ == held_.size()) end(earliest(), at, offset);
                emit(*out_, noteOn(offset(at), it->channel, it->note, it->velocity));
                held_[count_++] = {
                    .end = start + std::min(it->end, length), .channel = it->channel, .note = it->note};
            }
        }
    }

    std::size_t earliest() const noexcept {
        std::size_t best = 0;
        for (std::size_t i = 1; i < count_; ++i) {
            if (held_[i].end < held_[best].end) best = i;
        }
        return best;
    }

    // Ends the notes due before @p limit, in the order they are due.
    template <typename Offset>
    void endDue(double limit, const Offset& offset) noexcept {
        while (count_ > 0) {
            const std::size_t i = earliest();
            if (held_[i].end >= limit) return;
            end(i, held_[i].end, offset);
        }
    }

    template <typename Offset>
    void endAll(double at, const Offset& offset) noexcept {
        while (count_ > 0) end(earliest(), at, offset);
    }

    template <typename Offset>
    void end(std::size_t i, double at, const Offset& offset) noexcept {
        emit(*out_, noteOff(offset(at), held_[i].channel, held_[i].note));
        held_[i] = held_[--count_];
    }

    static constexpr double maxPasses = 256.0;
    // Beats within which a position is on a grid line (the engine's beatTolerance).
    static constexpr double lineTolerance = 1e-9;

    std::shared_ptr<clipBank> bank_;
    int slots_ = 8;
    double grid_ = 16.0;
    signal select_;
    std::shared_ptr<ePipe> out_;
    controlOutput playingOut_;
    controlOutput queuedOut_;
    int playing_ = -1;
    double launch_ = 0.0;                                     // the grid line the playing clip started on
    int meter_ = 4;                                           // the meter it started in, beats a bar
    double last_ = std::numeric_limits<double>::quiet_NaN();  // the last block's end, in beats
    std::array<heldNote, 64> held_{};                         // a 65th note ends the one due first
    std::size_t count_ = 0;
    bool fresh_ = true;  // took over no state and has not started or been changed
    int first_ = -2;     // the slot chosen in the first block; −2 before it
};

}  // namespace

std::span<const factoryEntry> clipFactories() {
    static const auto factories = std::to_array<factoryEntry>({
        {.type = "clipPlayer", .make = make<clipPlayer>},
    });
    return factories;
}

}  // namespace primitives
