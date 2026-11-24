/// @file
/// The table that owns every pipe and persists across plan swaps
/// (`primitives.md` §6).

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/pipe.h>
#include <espresso/primitives/primitive.h>

namespace primitives {

/// Owns all pipes (§6.2). Control and event pipes are allocated at
/// construction; each audio pipe when a plan that uses it is built
/// (make()), on the control thread, so the audio thread does not allocate,
/// and an engine holds only the audio pipes its plans have used.
class pipeTable {
public:
    /// Pipe counts per type and event pipe sizes.
    struct capacity {
        /// Audio pipes. Each holds `metadata::maxChannels` channels of a
        /// block twice, 16 KB at 256 samples, from the first plan that uses
        /// it; the 64 layered voices of the load piece use 915 (threads.md
        /// §9a.4).
        int audio = 1024;
        /// Control pipes, including the tempo pipe at index 0.
        int control = 4096;
        /// Event pipes.
        int event = 128;
        /// Events per event pipe per block.
        int eventsPerPipe = 256;
        /// Pool bytes per event pipe per block, for OSC messages.
        int poolBytes = 8192;
    };

    /// Allocates the control and event pipes for blocks of
    /// `info.blockSize`; audio pipes wait for make(). The tempo
    /// pipe starts at 120 BPM.
    pipeTable(const capacity& limits, const prepareInfo& info);

    /// Makes audio pipe @p ref if it is within the capacity and was not
    /// made yet; control and event pipes exist from construction. Control
    /// thread, before any plan that uses the pipe reaches the audio thread,
    /// which reads only its plans' pipes: a new pipe's slot is one no other
    /// thread reads.
    void make(metadata::pipeRef ref);
    /// The pipe @p ref, shared with the primitives that read or write it;
    /// null when the index is out of range or the pipe was not made.
    /// Control thread.
    std::shared_ptr<pipe> find(metadata::pipeRef ref);
    /// True when @p ref is within the table's capacity.
    bool contains(metadata::pipeRef ref) const noexcept ESPRESSO_NONBLOCKING;

    /// Audio pipe @p index; it must have been made (make()).
    aPipe& audio(std::int32_t index) noexcept { return *audio_[static_cast<std::size_t>(index)]; }
    /// Control pipe @p index; it must exist.
    cPipe& control(std::int32_t index) noexcept { return *control_[static_cast<std::size_t>(index)]; }
    /// Control pipe @p index, read-only; it must exist.
    const cPipe& control(std::int32_t index) const noexcept {
        return *control_[static_cast<std::size_t>(index)];
    }
    /// Event pipe @p index; it must exist.
    ePipe& events(std::int32_t index) noexcept { return *event_[static_cast<std::size_t>(index)]; }
    /// Audio pipe @p index, read-only; it must have been made.
    const aPipe& audio(std::int32_t index) const noexcept { return *audio_[static_cast<std::size_t>(index)]; }
    /// Event pipe @p index, read-only; it must exist.
    const ePipe& events(std::int32_t index) const noexcept {
        return *event_[static_cast<std::size_t>(index)];
    }

    /// Applies a plan's declaration of a pipe (audio thread).
    void configure(const metadata::pipeDecl& decl) noexcept ESPRESSO_NONBLOCKING;

    /// Starts a block of @p numSamples for the audio and event pipes among
    /// @p pipes (§10.2): zeroes the audio pipes and clears the event pipes.
    /// Other kinds are skipped. Pipes of different calls may start on
    /// different threads at once (`threads.md` §6.8).
    void beginBlock(std::span<const metadata::pipeRef> pipes, int numSamples) noexcept ESPRESSO_NONBLOCKING;
    /// Ends a block for the audio and control pipes among @p pipes:
    /// feedback pipes swap their buffers. Other kinds are skipped.
    void endBlock(std::span<const metadata::pipeRef> pipes) noexcept ESPRESSO_NONBLOCKING;

    /// Event writes dropped because a pipe or its pool was full.
    std::uint64_t overflows() const noexcept { return overflows_->load(std::memory_order_relaxed); }

private:
    // Shared with every event pipe, so a pipe that outlives the table
    // still counts into live memory.
    ePipe::counter overflows_ = std::make_shared<std::atomic<std::uint64_t>>(0);
    // Shared with the primitives bound to them, so a primitive's pipes
    // live as long as it does, and a pipe never refers into the table.
    std::vector<std::shared_ptr<aPipe>> audio_;
    std::vector<std::shared_ptr<cPipe>> control_;
    std::vector<std::shared_ptr<ePipe>> event_;
    prepareInfo info_;  // the block size of the pipes make() allocates
};

}  // namespace primitives
