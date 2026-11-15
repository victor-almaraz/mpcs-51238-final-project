/// @file
/// The table that owns every pipe and persists across plan swaps
/// (`primitives.md` §6).

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <proj/metadata/graph.h>
#include <proj/primitives/pipe.h>
#include <proj/primitives/primitive.h>

namespace primitives {

/// Owns all pipes. Allocates everything at construction (§6.2).
class pipeTable {
public:
    /// Pipe counts per type and event pipe sizes.
    struct capacity {
        /// Audio pipes.
        int audio = 256;
        /// Control pipes, including the tempo pipe at index 0.
        int control = 1024;
        /// Event pipes.
        int event = 128;
        /// Events per event pipe per block.
        int eventsPerPipe = 256;
        /// Pool bytes per event pipe per block, for OSC messages.
        int poolBytes = 8192;
    };

    /// Allocates every pipe for blocks of `info.blockSize`. The tempo
    /// pipe starts at 120 BPM.
    pipeTable(const capacity& limits, const prepareInfo& info);

    /// The pipe @p ref, shared with the primitives that read or write it;
    /// null when the index is out of range. Builder thread.
    std::shared_ptr<pipe> find(metadata::pipeRef ref);
    /// True when @p ref is within the table's capacity.
    bool contains(metadata::pipeRef ref) const noexcept PROJ_NONBLOCKING;

    /// Audio pipe @p index; it must exist.
    aPipe& audio(std::int32_t index) noexcept { return *audio_[static_cast<std::size_t>(index)]; }
    /// Control pipe @p index; it must exist.
    cPipe& control(std::int32_t index) noexcept { return *control_[static_cast<std::size_t>(index)]; }
    /// Control pipe @p index, read-only; it must exist.
    const cPipe& control(std::int32_t index) const noexcept { return *control_[static_cast<std::size_t>(index)]; }
    /// Event pipe @p index; it must exist.
    ePipe& events(std::int32_t index) noexcept { return *event_[static_cast<std::size_t>(index)]; }
    /// Audio pipe @p index, read-only; it must exist.
    const aPipe& audio(std::int32_t index) const noexcept { return *audio_[static_cast<std::size_t>(index)]; }
    /// Event pipe @p index, read-only; it must exist.
    const ePipe& events(std::int32_t index) const noexcept { return *event_[static_cast<std::size_t>(index)]; }

    /// Applies a plan's declaration of a pipe (audio thread).
    void configure(const metadata::pipeDecl& decl) noexcept PROJ_NONBLOCKING;

    /// Zeroes audio pipes and clears event pipes among @p used (§10.2).
    void beginBlock(std::span<const metadata::pipeRef> used, int numSamples) noexcept PROJ_NONBLOCKING;
    /// Swaps feedback buffers among @p used.
    void endBlock(std::span<const metadata::pipeRef> used) noexcept PROJ_NONBLOCKING;

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
};

} // namespace primitives
