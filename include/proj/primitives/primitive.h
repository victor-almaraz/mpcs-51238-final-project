/// @file
/// The primitive base class and the values passed to it
/// (`primitives.md` §4).

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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
};

/// Device audio buffers for one callback. Pointers may be null when the
/// engine runs without a device (offline render).
struct deviceBuffers {
    /// Input channels.
    const float* const* inputs = nullptr;
    /// Number of input channels.
    int numInputs = 0;
    /// Output channels. The engine clears them before the plan runs.
    float* const* outputs = nullptr;
    /// Number of output channels.
    int numOutputs = 0;
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
    /// Device buffers, or null.
    const deviceBuffers* device = nullptr;
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

    /// Allocates every buffer process() uses (builder thread).
    virtual void prepare(const prepareInfo& info) = 0;

    /// Takes over the state of @p outgoing, a primitive of the same type
    /// and shape from the outgoing plan (§9.2). Runs on the audio thread
    /// at the swap, so it swaps or copies preallocated buffers only.
    virtual void adoptState(primitive& outgoing) noexcept { (void)outgoing; }

    /// Processes one block (§4.4).
    virtual void process(const blockContext& ctx) noexcept = 0;

    /// Node ID.
    const std::string& id() const noexcept { return id_; }
    /// Primitive type.
    std::string_view type() const noexcept { return type_; }

    /// Sets the fade for the next block: output gain ramps from
    /// @p gainStart to @p gainEnd. A retiring primitive's control and
    /// event writes are discarded (§9.3.4). Called by the engine.
    void setFade(float gainStart, float gainEnd, bool retiring) noexcept {
        gainStart_ = gainStart;
        gainEnd_ = gainEnd;
        retiring_ = retiring;
    }

    /// True while the primitive is fading out.
    bool retiring() const noexcept { return retiring_; }

protected:
    /// Adds @p samples to @p channel of @p out with the fade applied.
    void emit(aPipe& out, int channel, std::span<const float> samples) noexcept;
    /// Sets @p channel of @p out, unless retiring.
    void emit(cPipe& out, int channel, float value) noexcept;
    /// Writes @p e to @p out, unless retiring.
    bool emit(ePipe& out, const event& e) noexcept;
    /// Adds @p samples to a device buffer with the fade applied.
    void emitDevice(float* out, std::span<const float> samples) noexcept;

private:
    std::string id_;
    std::string_view type_;
    float gainStart_ = 1.0f;
    float gainEnd_ = 1.0f;
    bool retiring_ = false;
};

} // namespace primitives
