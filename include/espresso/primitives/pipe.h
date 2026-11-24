/// @file
/// Pipes: audio, control, and event (`primitives.md` §5).

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/event.h>
#include <espresso/primitives/realtime.h>

namespace primitives {

/// Adds @p src into @p dst with a gain that ramps linearly from
/// @p gainStart to @p gainEnd across the samples (crossfades, §9.3). Stops
/// at the shorter span.
void addRamped(std::span<float> dst, std::span<const float> src, float gainStart,
               float gainEnd) noexcept ESPRESSO_NONBLOCKING;

/// Pipe interface (`primitives.md` §5.1). Primitives fetch a pipe's data
/// once per block and never call virtual methods per sample.
class pipe {
public:
    pipe() = default;
    virtual ~pipe() = default;
    // Pipes are shared through the pipe table, never copied or moved.
    pipe(const pipe&) = delete;
    pipe& operator=(const pipe&) = delete;
    pipe(pipe&&) = delete;
    pipe& operator=(pipe&&) = delete;

    /// The pipe's type.
    virtual metadata::pipeKind kind() const noexcept ESPRESSO_NONBLOCKING = 0;

    /// Active channel count, from 1 to `metadata::maxChannels`.
    int channels() const noexcept { return channels_; }
    /// True when the pipe delays its data by one block (§5.6).
    bool feedback() const noexcept { return feedback_; }

    /// Sets the channel count, clamped to 1 to `metadata::maxChannels`, and
    /// the feedback flag. Called on the audio thread when a plan becomes
    /// active; storage is preallocated.
    virtual void configure(int channels, bool feedback) noexcept ESPRESSO_NONBLOCKING;

protected:
    /// Active channel count.
    int channels_ = 1;
    /// Feedback flag.
    bool feedback_ = false;
};

/// Audio pipe: `channels` × block samples. Writers sum; the engine zeroes
/// the pipe at each block start (§5.2).
class aPipe final : public pipe {
public:
    /// Preallocates @p capacity channels (1 to `maxChannels`) of
    /// @p blockSize samples, twice when @p feedback is true (the second
    /// buffer serves feedback). A pipe table's pipes take any shape a plan
    /// declares; a plan's private pipe (`threads.md` §4.2) has its pipe's
    /// channels and no feedback.
    explicit aPipe(int blockSize, int capacity = metadata::maxChannels, bool feedback = true);

    /// Audio.
    metadata::pipeKind kind() const noexcept ESPRESSO_NONBLOCKING override {
        return metadata::pipeKind::audio;
    }
    /// Sets the channel count, at most the capacity, and the feedback flag,
    /// which a pipe without a second buffer ignores. A change of either
    /// clears both buffers, so no channel or feedback buffer replays old
    /// samples.
    void configure(int channels, bool feedback) noexcept ESPRESSO_NONBLOCKING override;

    /// Samples of @p channel in the current block; empty for a channel
    /// outside the capacity. A feedback pipe returns the previous block's
    /// data.
    std::span<const float> read(int channel) const noexcept ESPRESSO_NONBLOCKING;

    /// Adds @p src times @p gain into @p channel, or into the target's
    /// @p channel while the pipe forwards (forward()). A channel outside
    /// the active count is ignored.
    void add(int channel, std::span<const float> src, float gain = 1.0f) noexcept ESPRESSO_NONBLOCKING;
    /// Adds @p src into @p channel with a gain ramp from @p gainStart to
    /// @p gainEnd across the block (crossfades, §9.3), or into the target's
    /// @p channel while the pipe forwards (forward()). A channel outside the
    /// active count is ignored.
    void addRamp(int channel, std::span<const float> src, float gainStart,
                 float gainEnd) noexcept ESPRESSO_NONBLOCKING;

    /// Lets a private pipe (`threads.md` §4.2) forward to @p target, the
    /// table pipe its summing node adds it into, which outlives the plan
    /// that owns this pipe. The builder calls it for the private pipes of
    /// a group whose writers each add once a block (§4.2.5).
    void forwardTo(aPipe& target) noexcept { target_ = target; }
    /// Turns forwarding on or off; a pipe without a target never forwards.
    /// While it forwards, add() and addRamp() add into the target, the
    /// engine does not clear this pipe at a block's start, and its summing
    /// node skips it. The target then holds the bits the summing node
    /// would have left: a writer that adds w once into this pipe, cleared
    /// to +0, leaves 0 + w, which the summing node adds with gain 1 into a
    /// target that never holds −0, and p + (0 + w) equals p + w. That holds
    /// only while each writer adds once a block, nothing reads the target
    /// between the writers and the sum, and the writers and the sum run in
    /// plan order: one plan alone, with no crossfade and no shuffled order
    /// (`threads.md` §4.2.5). The engine turns it on and off for a whole
    /// plan, on the audio thread between blocks.
    void forward(bool on) noexcept ESPRESSO_NONBLOCKING { forwarding_ = on && target_.has_value(); }
    /// True while add() and addRamp() go to the target.
    bool forwarding() const noexcept { return forwarding_; }
    /// True when the pipe has a target (forwardTo()), so that it forwards
    /// whenever the engine turns forwarding on for its plan.
    bool hasTarget() const noexcept { return target_.has_value(); }

    /// Starts a block of @p numSamples: zeroes the buffer being written.
    void beginBlock(int numSamples) noexcept ESPRESSO_NONBLOCKING;
    /// Ends a block: a feedback pipe swaps its buffers.
    void endBlock() noexcept ESPRESSO_NONBLOCKING;

private:
    // A feedback pipe reads one buffer while writers fill the other and
    // swaps them at each block end; any other pipe uses the first only.
    std::span<const float> readBuffer() const noexcept { return buffer(feedback_ && swapped_); }
    std::span<float> writeBuffer() noexcept { return buffer(feedback_ && !swapped_); }
    // The first buffer, or the second when @p second is true (only a
    // pipe with two has a second).
    std::span<float> buffer(bool second) const noexcept {
        const auto size = samples_.size() / static_cast<std::size_t>(buffers_);
        return samples_.subspan(second ? size : 0, size);
    }

    // The samples of @p channel in @p buffer.
    template <typename Sample>
    std::span<Sample> channelOf(std::span<Sample> buffer, int channel) const noexcept {
        const auto size = static_cast<std::size_t>(blockSize_);
        return buffer.subspan(static_cast<std::size_t>(channel) * size, size);
    }
    // The part of @p channel this block writes, or empty when the channel
    // is not active; while the pipe forwards, the target's.
    std::span<float> written(int channel) noexcept ESPRESSO_NONBLOCKING;

    // First, beside the channel count of pipe: a write to a pipe that
    // forwards reads those three from this pipe and nothing else, so it
    // takes one cache line of it rather than two. A plan's many private
    // pipes are otherwise untouched while they forward, so that line is
    // rarely cached. Only a pipe that forwards reads target_, its group's
    // table pipe, set when the group may forward.
    bool forwarding_ = false;
    std::optional<std::reference_wrapper<aPipe>> target_;
    int blockSize_ = 0;
    int numSamples_ = 0;
    int capacity_ = metadata::maxChannels;
    int buffers_ = 2;
    // Every buffer, `capacity_` channels each, zeroed by one memset: a
    // vector's element-wise construction made a default pipe table take
    // 85 ms instead of 5 ms in Debug builds (modern-cpp skill §9).
    std::unique_ptr<float[]> owner_;  // NOLINT(modernize-avoid-c-arrays): skill §9
    std::span<float> samples_;
    bool swapped_ = false;
};

/// Control pipe: one value per channel. A write replaces the value, which
/// holds until the next write (§5.3). Values are atomics so other threads
/// can read them (`/c.get`).
class cPipe final : public pipe {
public:
    /// Control.
    metadata::pipeKind kind() const noexcept ESPRESSO_NONBLOCKING override {
        return metadata::pipeKind::control;
    }
    /// Sets the channel count and feedback flag. Channels that become
    /// active take channel 0's value, which a reader of the narrower pipe
    /// saw on every channel; a feedback change carries the current values
    /// into both halves, so the next block end publishes no stale value.
    void configure(int channels, bool feedback) noexcept ESPRESSO_NONBLOCKING override;

    /// The value of @p channel. A feedback pipe returns the value written
    /// in the previous block.
    float value(int channel = 0) const noexcept {
        return front_[clampChannel(channel)].load(std::memory_order_relaxed);
    }

    /// Sets @p channel to @p v.
    void set(int channel, float v) noexcept ESPRESSO_NONBLOCKING;
    /// Sets every channel to @p v.
    void setAll(float v) noexcept ESPRESSO_NONBLOCKING;

    /// Ends a block: a feedback pipe publishes the values written in it.
    void endBlock() noexcept ESPRESSO_NONBLOCKING;

private:
    static std::size_t clampChannel(int c) noexcept {
        return static_cast<std::size_t>(c < 0 ? 0
                                              : (c >= metadata::maxChannels ? metadata::maxChannels - 1 : c));
    }

    // Value-initialized: every value starts at 0.
    static_assert(realtimeAtomic<float>);
    std::array<std::atomic<float>, metadata::maxChannels> front_{};
    std::array<std::atomic<float>, metadata::maxChannels> back_{};
};

/// Event pipe: a bounded array of events sorted by sample offset, plus a
/// byte pool for OSC messages. Cleared at each block start (§5.4).
class ePipe final : public pipe {
public:
    /// An overflow counter shared by a table and its event pipes.
    using counter = std::shared_ptr<std::atomic<std::uint64_t>>;
    static_assert(realtimeAtomic<std::uint64_t>);

    /// Preallocates @p capacity events and @p poolBytes of pool.
    /// Overflows increment @p overflowCounter, which the pipe shares.
    ePipe(std::size_t capacity, std::size_t poolBytes, counter overflowCounter);

    /// Event.
    metadata::pipeKind kind() const noexcept ESPRESSO_NONBLOCKING override {
        return metadata::pipeKind::event;
    }

    /// This block's events, sorted by sample offset. Equal offsets keep
    /// writer order (§5.4.2).
    std::span<const event> events() const noexcept { return events_.first(count_); }

    /// Inserts @p e in offset order. When full, drops it, counts an
    /// overflow, and returns false (§5.4.3).
    bool write(const event& e) noexcept ESPRESSO_NONBLOCKING;

    /// Copies an OSC message into the pool and writes an OSC event for
    /// it. Drops and counts it when the pool or the array is full.
    bool writeOsc(std::uint32_t offset, std::span<const std::byte> message) noexcept ESPRESSO_NONBLOCKING;

    /// The pool bytes of @p ref.
    std::span<const std::byte> data(poolRef ref) const noexcept ESPRESSO_NONBLOCKING;

    /// Copies @p e, and its pool data for an OSC event, from @p from.
    bool copyFrom(const ePipe& from, const event& e) noexcept ESPRESSO_NONBLOCKING;

    /// Starts a block: clears the events and the pool.
    void beginBlock() noexcept ESPRESSO_NONBLOCKING;

private:
    std::optional<poolRef> store(std::span<const std::byte> bytes) noexcept ESPRESSO_NONBLOCKING;
    void overflow() noexcept ESPRESSO_NONBLOCKING;

    // Value-initialized arrays, not vectors, whose element-wise construction
    // took 25 ms a table in Debug (modern-cpp skill §9). Construction writes
    // every page, so the audio thread's first writes take no page fault.
    std::unique_ptr<event[]> eventStore_;     // NOLINT(modernize-avoid-c-arrays): skill §9
    std::unique_ptr<std::byte[]> poolStore_;  // NOLINT(modernize-avoid-c-arrays): skill §9
    std::span<event> events_;
    std::size_t count_ = 0;
    std::span<std::byte> pool_;
    std::size_t poolUsed_ = 0;
    counter overflows_;
};

}  // namespace primitives
