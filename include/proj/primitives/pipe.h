/// @file
/// Pipes: audio, control, and event (`primitives.md` §5). Video pipes
/// arrive with the image primitives (M3).

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <proj/metadata/graph.h>
#include <proj/primitives/event.h>

namespace primitives {

/// Pipe interface (`primitives.md` §5.1). Primitives fetch a pipe's data
/// once per block and never call virtual methods per sample.
class pipe {
public:
    virtual ~pipe() = default;

    /// The pipe's type.
    virtual metadata::pipeKind kind() const noexcept = 0;

    /// Active channel count, from 1 to `metadata::maxChannels`.
    int channels() const noexcept { return channels_; }
    /// True when the pipe delays its data by one block (§5.6).
    bool feedback() const noexcept { return feedback_; }

    /// Sets the channel count and feedback flag. Called on the audio
    /// thread when a plan becomes active; storage is preallocated.
    virtual void configure(int channels, bool feedback) noexcept;

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
    /// Preallocates `maxChannels` channels of @p blockSize samples, twice
    /// (the second buffer serves feedback).
    explicit aPipe(int blockSize);

    /// Audio.
    metadata::pipeKind kind() const noexcept override { return metadata::pipeKind::audio; }
    /// Sets the channel count and feedback flag.
    void configure(int channels, bool feedback) noexcept override;

    /// Samples of @p channel in the current block. A feedback pipe
    /// returns the previous block's data.
    std::span<const float> read(int channel) const noexcept;

    /// Adds @p src times @p gain into @p channel.
    void add(int channel, std::span<const float> src, float gain = 1.0f) noexcept;
    /// Adds @p src into @p channel with a gain ramp from @p gainStart to
    /// @p gainEnd across the block (crossfades, §9.3).
    void addRamp(int channel, std::span<const float> src, float gainStart, float gainEnd) noexcept;

    /// Starts a block of @p numSamples: zeroes the buffer being written.
    void beginBlock(int numSamples) noexcept;
    /// Ends a block: a feedback pipe swaps its buffers.
    void endBlock() noexcept;

private:
    // A feedback pipe reads one buffer while writers fill the other and
    // swaps them at each block end; any other pipe uses the first only.
    std::vector<float>& readBuffer() noexcept { return feedback_ && swapped_ ? second_ : first_; }
    const std::vector<float>& readBuffer() const noexcept { return feedback_ && swapped_ ? second_ : first_; }
    std::vector<float>& writeBuffer() noexcept { return !feedback_ ? first_ : (swapped_ ? first_ : second_); }

    std::span<float> channelOf(std::vector<float>& buffer, int channel) const noexcept {
        return std::span(buffer).subspan(static_cast<std::size_t>(channel) * static_cast<std::size_t>(blockSize_),
                                         static_cast<std::size_t>(blockSize_));
    }
    std::span<const float> channelOf(const std::vector<float>& buffer, int channel) const noexcept {
        return std::span(buffer).subspan(static_cast<std::size_t>(channel) * static_cast<std::size_t>(blockSize_),
                                         static_cast<std::size_t>(blockSize_));
    }

    int blockSize_;
    int numSamples_;
    std::vector<float> first_;
    std::vector<float> second_;
    bool swapped_ = false;
};

/// Control pipe: one value per channel. A write replaces the value, which
/// holds until the next write (§5.3). Values are atomics so other threads
/// can read them (`/c.get`).
class cPipe final : public pipe {
public:
    cPipe() noexcept;

    /// Control.
    metadata::pipeKind kind() const noexcept override { return metadata::pipeKind::control; }

    /// The value of @p channel. A feedback pipe returns the value written
    /// in the previous block.
    float value(int channel = 0) const noexcept { return front_[clampChannel(channel)].load(std::memory_order_relaxed); }

    /// Sets @p channel to @p v.
    void set(int channel, float v) noexcept;
    /// Sets every channel to @p v.
    void setAll(float v) noexcept;

    /// Ends a block: a feedback pipe publishes the values written in it.
    void endBlock() noexcept;

private:
    static std::size_t clampChannel(int c) noexcept {
        return static_cast<std::size_t>(c < 0 ? 0 : (c >= metadata::maxChannels ? metadata::maxChannels - 1 : c));
    }

    std::array<std::atomic<float>, metadata::maxChannels> front_;
    std::array<std::atomic<float>, metadata::maxChannels> back_;
};

/// Event pipe: a bounded array of events sorted by sample offset, plus a
/// byte pool for OSC messages. Cleared at each block start (§5.4).
class ePipe final : public pipe {
public:
    /// Preallocates @p capacity events and @p poolBytes of pool.
    /// Overflows increment @p overflowCounter, which must outlive the pipe.
    ePipe(std::size_t capacity, std::size_t poolBytes, std::atomic<std::uint64_t>& overflowCounter);

    /// Event.
    metadata::pipeKind kind() const noexcept override { return metadata::pipeKind::event; }

    /// This block's events, sorted by sample offset. Equal offsets keep
    /// writer order (§5.4.2).
    std::span<const event> events() const noexcept { return std::span(events_).first(count_); }

    /// Inserts @p e in offset order. When full, drops it, counts an
    /// overflow, and returns false (§5.4.3).
    bool write(const event& e) noexcept;

    /// Copies an OSC message into the pool and writes an OSC event for
    /// it. Drops and counts it when the pool or the array is full.
    bool writeOsc(std::uint32_t offset, std::span<const std::byte> message) noexcept;

    /// The pool bytes of @p ref.
    std::span<const std::byte> data(poolRef ref) const noexcept;

    /// Copies @p e, and its pool data for an OSC event, from @p from.
    bool copyFrom(const ePipe& from, const event& e) noexcept;

    /// Starts a block: clears the events and the pool.
    void beginBlock() noexcept;

private:
    std::optional<poolRef> store(std::span<const std::byte> bytes) noexcept;
    void overflow() noexcept;

    std::vector<event> events_;
    std::size_t count_ = 0;
    std::vector<std::byte> pool_;
    std::size_t poolUsed_ = 0;
    std::atomic<std::uint64_t>& overflows_;
};

} // namespace primitives
