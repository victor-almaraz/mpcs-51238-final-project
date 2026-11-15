/// @file
/// The primitive base class and the values passed to it
/// (`primitives.md` §4).

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <proj/primitives/realtime.h>

namespace primitives {

class aPipe;
class cPipe;
class ePipe;
struct event;

/// Values fixed for a plan's lifetime (§4.2).
struct prepareInfo {
    /// Sample rate, in hertz.
    double sampleRate = 48000.0;
    /// Largest block, in samples.
    int blockSize = 256;
    /// Video frame width, in pixels.
    int frameWidth = 1280;
    /// Video frame height, in pixels.
    int frameHeight = 720;
};

/// Most device channels a callback passes to the engine.
inline constexpr int maxDeviceChannels = 64;

/// Device audio buffers for one callback: one span per channel, each as
/// long as the callback. Empty when the engine runs without a device
/// (offline render); a channel's span is empty when the device has none.
struct deviceBuffers {
    /// Input channels.
    std::span<const std::span<const float>> inputs;
    /// Output channels. The engine clears them before the plan runs.
    std::span<const std::span<float>> outputs;
};

/// What a primitive sees during one block (§4.4).
struct blockContext {
    /// Engine sample clock at the block start.
    std::int64_t blockStartSample = 0;
    /// Samples in this block, at most `prepareInfo::blockSize`.
    int numSamples = 0;
    /// Sample rate, in hertz.
    double sampleRate = 48000.0;
    /// Beat position at the block start (§7.5).
    double beat = 0.0;
    /// Tempo read at the block start, in beats per minute.
    double tempo = 120.0;
    /// Beats per bar.
    int meter = 4;
    /// Device clock at the block start, in seconds. Sources convert
    /// receive timestamps to sample offsets against it (§7.2.2).
    double hostTime = 0.0;
    /// Device buffers.
    deviceBuffers device;
    /// Index of this block's first frame within the device buffers.
    int deviceOffset = 0;

    /// Beats advanced per sample at this block's tempo.
    double beatsPerSample() const noexcept { return tempo / 60.0 / sampleRate; }
};

/// Base of every source, sink, and generator (§4.1). The builder
/// constructs and prepares a primitive; the audio thread calls
/// adoptState() at a swap and process() once per block; the message
/// thread destroys it.
class primitive {
public:
    /// A primitive with node ID @p id of type @p type. @p type must
    /// outlive the primitive (it names a `metadata` table entry).
    primitive(std::string id, std::string_view type) : id_(std::move(id)), type_(type) {}
    virtual ~primitive() = default;

    primitive(const primitive&) = delete;
    primitive& operator=(const primitive&) = delete;

    /// Allocates every buffer process() uses (builder thread). The
    /// built-in primitives allocate when constructed, on the same thread,
    /// so the default does nothing.
    virtual void prepare(const prepareInfo& info) { (void)info; }

    /// Takes over the state of @p outgoing, a primitive of the same type
    /// and shape from the outgoing plan (§9.2). Runs on the audio thread
    /// at the swap, so it swaps or copies preallocated buffers only.
    virtual void adoptState(primitive& outgoing) noexcept PROJ_NONBLOCKING { (void)outgoing; }

    /// Processes one block (§4.4).
    virtual void process(const blockContext& ctx) noexcept PROJ_NONBLOCKING = 0;

    /// Node ID.
    const std::string& id() const noexcept { return id_; }
    /// Primitive type.
    std::string_view type() const noexcept { return type_; }

    /// Sets the fade for the next block: output gain ramps from
    /// @p gainStart to @p gainEnd. A retiring primitive's control writes
    /// and its events other than note-offs are discarded (§9.3.4).
    /// @p finalBlock marks a retiring primitive's last block. Called by
    /// the engine.
    void setFade(float gainStart, float gainEnd, bool retiring,
                 bool finalBlock = false) noexcept PROJ_NONBLOCKING {
        gainStart_ = gainStart;
        gainEnd_ = gainEnd;
        retiring_ = retiring;
        finalBlock_ = finalBlock;
    }

    /// True while the primitive is fading out.
    bool retiring() const noexcept { return retiring_; }
    /// True in a retiring primitive's last block: the time to end held notes.
    bool finalBlock() const noexcept { return finalBlock_; }
    /// True when @p e may be written: always, or only note-offs while
    /// retiring, so notes held across a swap still end.
    bool passes(const event& e) const noexcept PROJ_NONBLOCKING;

protected:
    /// Adds @p samples to @p channel of @p out with the fade applied.
    void emit(aPipe& out, int channel, std::span<const float> samples) noexcept PROJ_NONBLOCKING;
    /// Sets @p channel of @p out, unless retiring.
    void emit(cPipe& out, int channel, float value) noexcept PROJ_NONBLOCKING;
    /// Writes @p e to @p out, unless retiring; a retiring primitive still
    /// writes note-offs (passes()).
    bool emit(ePipe& out, const event& e) noexcept PROJ_NONBLOCKING;
    /// Copies @p e, with its OSC data, from @p from to @p out when it
    /// passes().
    bool forward(ePipe& out, const ePipe& from, const event& e) noexcept PROJ_NONBLOCKING;
    /// Writes the OSC message @p message to @p out at @p offset, unless
    /// retiring.
    bool emitOsc(ePipe& out, std::uint32_t offset,
                 std::span<const std::byte> message) noexcept PROJ_NONBLOCKING;
    /// Adds @p samples to a device buffer with the fade applied.
    void emitDevice(std::span<float> out, std::span<const float> samples) noexcept PROJ_NONBLOCKING;

private:
    std::string id_;
    std::string_view type_;
    float gainStart_ = 1.0f;
    float gainEnd_ = 1.0f;
    bool retiring_ = false;
    bool finalBlock_ = false;
};

} // namespace primitives
