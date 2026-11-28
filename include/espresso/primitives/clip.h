/// @file
/// Clips (`canvas.md` §5): note sequences in beats read from Standard MIDI
/// Files, held in banks the engine keeps across plan swaps, and handed to
/// the node that plays them without allocating or freeing on the thread
/// that runs it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/realtime.h>
#include <espresso/primitives/spscQueue.h>

namespace primitives {

/// One note of a clip, in beats from the clip's start.
struct clipNote {
    /// Its note-on.
    double start = 0.0;
    /// Its note-off, after `start`.
    double end = 0.0;
    /// MIDI channel, 0 to 15.
    std::uint8_t channel = 0;
    /// MIDI note, 0 to 127.
    std::uint8_t note = 0;
    /// Velocity, 1 to 127.
    std::uint8_t velocity = 0;

    /// Equal when every field is equal.
    friend bool operator==(const clipNote&, const clipNote&) = default;
};

/// A note sequence, in beats: what a MIDI file holds.
struct clip {
    /// The notes, sorted by start.
    std::vector<clipNote> notes;
    /// Its length in beats: the end of its last track, at least its last
    /// note-off.
    double length = 0.0;
};

/// Most notes a clip holds.
inline constexpr std::size_t maxClipNotes = 16384;

/// Reads the Standard MIDI File @p bytes, format 0 or 1, into a clip: its
/// tracks merged, a quarter note a beat by the file's division, its tempo
/// and meter events ignored. A note-on of velocity 0 is a note-off; a note
/// still held at the end of its track ends there. Nothing, with @p error
/// set, for bytes that are not such a file, a file in SMPTE time, or more
/// than maxClipNotes notes.
std::optional<clip> readMidiFile(std::span<const std::uint8_t> bytes, std::string& error);

/// Most slots a clip bank holds.
inline constexpr int maxClipSlots = 64;

/// The slots of one bank of clips, shared by the control thread, which
/// gives it clips, and the node that plays it, which takes them at the
/// start of a block (`canvas.md` §5.4). A clip a slot gives up goes back to
/// the control thread, which frees it, so the node's thread never frees.
class clipBank {
public:
    /// Clips given and not yet taken back, at most.
    static constexpr std::size_t queueSize = 64;

    clipBank() = default;
    clipBank(const clipBank&) = delete;
    clipBank& operator=(const clipBank&) = delete;
    clipBank(clipBank&&) = delete;
    clipBank& operator=(clipBank&&) = delete;
    ~clipBank() = default;

    /// Control thread: gives slot @p slot, from 0, the clip @p c, or none
    /// when @p c is null. False, giving nothing, for a slot out of range or
    /// while queueSize changes wait to be taken back (collect()).
    [[nodiscard]] bool offer(int slot, std::unique_ptr<const clip> c);
    /// Control thread: true while offer() can take a change: fewer than
    /// queueSize wait to be taken back.
    bool accepting() const noexcept { return inFlight_ < queueSize; }
    /// Control thread: frees the clips the bank's node gave back.
    void collect();

    /// The thread that runs the bank's node: puts the clips offered since
    /// the last call in their slots, giving back those they replace.
    void update() noexcept ESPRESSO_NONBLOCKING;
    /// The thread that runs the bank's node: slot @p slot's clip, if it
    /// holds one.
    metadata::optionalRef<clip> at(int slot) const noexcept ESPRESSO_NONBLOCKING;

private:
    struct change {
        int slot = 0;
        std::unique_ptr<const clip> item;
    };
    // The queues first: their ends are aligned to cache lines.
    spscQueue<change> incoming_{queueSize};
    // One item for each change taken, the clip it replaced or none, so it
    // never fills: inFlight_ bounds the changes not yet given back.
    spscQueue<std::unique_ptr<const clip>> outgoing_{queueSize};
    std::size_t inFlight_ = 0;  // control thread
    // The change update() is taking, a member so that update() destroys
    // none; empty between calls.
    change taken_;
    std::array<std::unique_ptr<const clip>, maxClipSlots> slots_;
};

/// The engine's clip banks by name, kept across plans, so a bank's clips
/// stay loaded while the plans that play them change. Control thread.
class clipBanks {
public:
    /// The bank named @p name, made empty the first time.
    std::shared_ptr<clipBank> bank(const std::string& name);
    /// The names of the banks made so far, sorted.
    std::vector<std::string> names() const;
    /// Frees the clips every bank's node gave back.
    void collect();

private:
    std::map<std::string, std::shared_ptr<clipBank>> banks_;
};

}  // namespace primitives
