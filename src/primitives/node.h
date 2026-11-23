// Internal: what a primitive's constructor reads from its node, and the
// per-block helpers that primitives share. Not part of the public API;
// primitives are created through buildPlan().

#pragma once

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <complex>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/metadata/primitiveSpec.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/event.h>
#include <espresso/primitives/io.h>
#include <espresso/primitives/pipeTable.h>
#include <espresso/primitives/plan.h>
#include <espresso/primitives/primitive.h>
#include <espresso/primitives/random.h>
#include <espresso/primitives/realtime.h>

namespace primitives {

using metadata::maxChannels;

// --- Shared values --------------------------------------------------------

inline constexpr double pi = std::numbers::pi;
inline constexpr double twoPi = 2.0 * pi;
inline constexpr float notANumber = std::numeric_limits<float>::quiet_NaN();

// An array with every element @p value.
template <typename T, std::size_t N>
constexpr std::array<T, N> filledArray(const T& value) {
    std::array<T, N> out{};
    out.fill(value);
    return out;
}

// --- Finite state -----------------------------------------------------------

// True when every number in a state value is finite. A NaN or an infinity
// in adopted state would keep a node silent (or muted) after the patch
// that caused it is fixed, so state is adopted only when finite (§9.2).
inline bool isFinite(float x) noexcept { return std::isfinite(x); }
inline bool isFinite(double x) noexcept { return std::isfinite(x); }
inline bool isFinite(std::complex<float> x) noexcept {
    return std::isfinite(x.real()) && std::isfinite(x.imag());
}
template <typename T>
bool isFinite(const std::vector<T>& v) noexcept;
template <typename A, typename B>
bool isFinite(const std::pair<A, B>& p) noexcept;
template <typename T, std::size_t N>
bool isFinite(const std::array<T, N>& a) noexcept;
// A state type with its own check.
template <typename T>
    requires requires(const T& t) { t.isFinite(); }
bool isFinite(const T& t) noexcept {
    return t.isFinite();
}

template <typename A, typename B>
bool isFinite(const std::pair<A, B>& p) noexcept {
    return isFinite(p.first) && isFinite(p.second);
}

template <typename T, std::size_t N>
bool isFinite(const std::array<T, N>& a) noexcept {
    return std::all_of(a.begin(), a.end(), [](const T& x) { return isFinite(x); });
}

template <typename T>
bool isFinite(const std::vector<T>& v) noexcept {
    return std::all_of(v.begin(), v.end(), [](const T& x) { return isFinite(x); });
}

template <typename... T>
bool allFinite(const T&... values) noexcept {
    return (isFinite(values) && ...);
}

// Copies @p theirs into @p mine when every number in it is finite.
template <typename T>
void adoptFinite(T& mine, const T& theirs) noexcept {
    if (isFinite(theirs)) mine = theirs;
}

// Swaps two state containers when their sizes match (adoption, §9.2).
template <typename T>
void adoptIfSameSize(T& mine, T& theirs) noexcept {
    if (mine.size() == theirs.size()) std::swap(mine, theirs);
}

// --- Interpolation ------------------------------------------------------------

// 4-point Hermite interpolation between @p x0 and @p x1 at @p frac, from 0
// to 1, with their neighbours @p xm1 and @p x2.
inline float hermite(float xm1, float x0, float x1, float x2, float frac) noexcept ESPRESSO_NONBLOCKING {
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - (2.5f * x0) + (2.0f * x1) - (0.5f * x2);
    const float c3 = (0.5f * (x2 - xm1)) + (1.5f * (x0 - x1));
    return (((((c3 * frac) + c2) * frac) + c1) * frac) + x0;
}

// @p x limited to [@p lo, @p hi], where lo <= hi, by std::fmax and then
// std::fmin, which treat a NaN as missing: a NaN @p x gives @p lo, where
// std::clamp would pass it through.
template <std::floating_point T>
T fclamp(T x, T lo, T hi) noexcept ESPRESSO_NONBLOCKING {
    return std::fmin(std::fmax(x, lo), hi);
}

// The samples in @p seconds at @p sampleRate, at least one.
inline int samplesFor(double seconds, double sampleRate) noexcept {
    return std::max(1, static_cast<int>(std::lround(seconds * sampleRate)));
}

// --- Block buffers ------------------------------------------------------------

// Scratch samples for one block, allocated when constructed on the
// builder thread with the plan's block size.
class blockBuffer {
public:
    blockBuffer() = default;
    explicit blockBuffer(int blockSize) : samples_(static_cast<std::size_t>(std::max(blockSize, 0)), 0.0f) {}

    float& operator[](std::size_t i) noexcept { return samples_[i]; }
    std::span<float> first(std::size_t n) noexcept { return std::span(samples_).first(n); }

private:
    std::vector<float> samples_;
};

// --- Values a video node reads --------------------------------------------

// A parameter of a video-domain node (primitives.md §7.7): a static
// value, a control pipe, or an audio pipe's latest sample (§8.1). Read
// once per frame on the render thread; both pipes publish atomics, so
// the read races with nothing on the thread that runs the writing node.
class frameValue {
public:
    frameValue() = default;
    explicit frameValue(float constant) : constant_(constant) {}

    // Follows control pipe @p p, or audio pipe @p p's latest sample.
    void bind(std::shared_ptr<const cPipe> p) noexcept { control_ = std::move(p); }
    void bind(std::shared_ptr<const aPipe> p) noexcept { audio_ = std::move(p); }

    // The value now: channel 0 of the pipe it follows, or the constant.
    float value() const noexcept {
        if (control_) return control_->value(0);
        if (audio_) return audio_->latest(0);
        return constant_;
    }

private:
    float constant_ = 0.0f;
    std::shared_ptr<const cPipe> control_;
    std::shared_ptr<const aPipe> audio_;
};

// A color parameter of an image generator (§7.7.4): static (or none), or
// following a 4-channel control pipe, R, G, B, A. Read once per frame or
// block; control values are atomic.
class frameColor {
public:
    frameColor() = default;
    explicit frameColor(std::optional<metadata::rgba> constant) : constant_(constant) {}
    explicit frameColor(std::shared_ptr<const cPipe> p) : pipe_(std::move(p)) {}

    // The color now, or none.
    std::optional<metadata::rgba> value() const noexcept {
        if (!pipe_) return constant_;
        return metadata::rgba{pipe_->value(0), pipe_->value(1), pipe_->value(2), pipe_->value(3)};
    }

private:
    std::optional<metadata::rgba> constant_;
    std::shared_ptr<const cPipe> pipe_;
};

// --- Signals ------------------------------------------------------------

// A parameter or input that is a static value, a control pipe, or an
// audio pipe (primitives.md §7.1.2). Read it once per channel per block.
// A pipe with fewer channels than requested wraps: channel c reads
// channel c mod channels, so a mono pipe feeds every channel.
class signal {
public:
    signal() = default;
    // The constant @p constant, read in blocks of up to @p blockSize.
    signal(float constant, int blockSize) : constant_(constant), scratch_(blockSize) {}

    void bind(std::shared_ptr<const aPipe> p) noexcept { audio_ = std::move(p); }
    void bind(std::shared_ptr<const cPipe> p) noexcept { control_ = std::move(p); }

    bool isAudio() const noexcept { return audio_ != nullptr; }
    // True when the signal is a constant: the parameter or input is not bound to a pipe.
    bool isStatic() const noexcept { return !audio_ && !control_; }
    // The bound pipe's channels as they are now. A plan's pipe declarations
    // take effect only when it becomes active (primitives.md §6.5), so a
    // node being built sees the shape the playing plan gave the pipe: size
    // state by nodeContext::inputChannels(), and read this in process().
    int channels() const noexcept {
        return audio_ ? audio_->channels() : (control_ ? control_->channels() : 1);
    }

    // The block-rate value of @p ch: the constant, the control value, or
    // the audio pipe's last sample.
    float value(int ch = 0) const noexcept {
        if (control_) return control_->value(mapChannel(ch));
        if (audio_) {
            const auto s = audio_->read(mapChannel(ch));
            return s.empty() ? 0.0f : s.back();
        }
        return constant_;
    }

    // Per-sample values of @p ch for @p n samples. Control values ramp
    // linearly from the previous block's value (§5.3.3).
    std::span<const float> block(int ch, int n) noexcept {
        const auto count = static_cast<std::size_t>(n);
        if (audio_) return audio_->read(mapChannel(ch)).first(count);
        const float target = value(ch);
        float& last = last_[static_cast<std::size_t>(ch)];
        if (std::isnan(last) || !control_) last = target;
        if (last == target) {
            // Constant over the block: the scratch already holds it unless
            // the value or the block grew since the last fill.
            if (filledWith_ != target || filledCount_ < count) {
                std::fill_n(scratch_.first(count).begin(), count, target);
                filledWith_ = target;
                filledCount_ = count;
            }
            return scratch_.first(count);
        }
        const float step = (target - last) / static_cast<float>(n > 0 ? n : 1);
        const auto out = scratch_.first(count);
        for (std::size_t i = 0; i < count; ++i) out[i] = last + step * static_cast<float>(i + 1);
        last = target;
        filledWith_ = notANumber;
        return out;
    }

private:
    int mapChannel(int ch) const noexcept { return ch % channels(); }

    float constant_ = 0.0f;
    std::shared_ptr<const aPipe> audio_;
    std::shared_ptr<const cPipe> control_;
    blockBuffer scratch_;
    float filledWith_ = notANumber;  // the value every scratch sample holds, or NaN
    std::size_t filledCount_ = 0;
    // Each channel's value at the previous block, or NaN before the first.
    std::array<float, maxChannels> last_ = filledArray<float, maxChannels>(notANumber);
};

// --- Outputs ------------------------------------------------------------------

// A pipe a primitive writes, with the channel count its plan declares.
template <typename P>
struct output {
    std::shared_ptr<P> pipe;
    int channels = 1;
};

using audioOutput = output<aPipe>;
using controlOutput = output<cPipe>;

// An output that is an audio or a control pipe.
struct signalOut {
    std::shared_ptr<aPipe> audio;
    std::shared_ptr<cPipe> control;
    int channels = 1;
};

// --- Notes ------------------------------------------------------------------

// Notes held, per MIDI channel and note number.
class heldNotes {
public:
    // Records a note-on or note-off; other events are ignored.
    void update(const event& e) noexcept {
        if (e.type != eventType::noteOn && e.type != eventType::noteOff) return;
        // Masked, so a malformed payload cannot index past the set.
        const auto at = (static_cast<std::size_t>(channel(e)) & 0x0fU) * notes +
                        (static_cast<std::size_t>(data1(e)) & 0x7fU);
        held_[at] = e.type == eventType::noteOn;
    }
    bool any() const noexcept { return held_.any(); }

private:
    static constexpr std::size_t channels = 16;
    static constexpr std::size_t notes = 128;
    std::bitset<channels * notes> held_;
};

// --- Node context -------------------------------------------------------

// What a node reports faults with from process() (ioHub::reportFault),
// under its own ID. Only nodeContext::reportFaults() makes one, and it
// declares the reports as it does: the fault queue takes one producer at
// a time, so every node that reports runs one at a time with the others,
// in plan order (threads.md §4.1).
class faultReporter {
public:
    // Reports @p what about the node; a full queue drops the report but
    // still counts it.
    void operator()(std::string_view what) const noexcept ESPRESSO_NONBLOCKING {
        io_.get().reportFault(node_, what);
    }

private:
    friend class nodeContext;
    faultReporter(ioHub& io, std::string node) : io_(io), node_(std::move(node)) {}

    std::reference_wrapper<ioHub> io_;
    std::string node_;
};

class frameRing;
struct wavetableSet;

// What the nodes of one plan share while it is built: its buffers by
// index (samples.md §3.1), the frame ring of each frame pipe (§10.3), the
// band-limited tables of each buffer and table size (§9.2), and the
// memory its nodes have reserved (§5.6).
struct planBuild {
    std::map<std::int32_t, std::shared_ptr<sampleBuffer>> buffers;
    std::map<metadata::pipeRef, std::shared_ptr<frameRing>> frames;
    std::map<std::pair<std::int32_t, std::size_t>, std::shared_ptr<const wavetableSet>> wavetables;
    std::size_t live = 0;      // bytes of the live buffers, this plan's included
    std::size_t reserved = 0;  // bytes
};

// A node's private pipes, by the audio pipe each replaces among its
// outputs (threads.md §4.2).
using privateOutputMap = std::map<metadata::pipeRef, std::shared_ptr<aPipe>>;

// A frame port's pipe and the ring its frames are in.
struct frameOutput {
    std::shared_ptr<ePipe> events;
    std::shared_ptr<frameRing> ring;
};
struct frameInput {
    std::shared_ptr<const ePipe> events;
    std::shared_ptr<const frameRing> ring;
};

// Reads a node's parameters and ports, resolving pipes in the table.
// Throws buildError for anything the engine cannot run.
class nodeContext {
public:
    // @p declared indexes @p graph's pipe declarations. @p chainEnds maps
    // each link of a drawing chain to the video pipe the chain ends in
    // (§5.5.5); a port bound to a link binds the end pipe. An output bound
    // to a pipe among @p privateOutputs binds its private pipe instead.
    nodeContext(const metadata::nodeSpec& node, const metadata::primitiveSpec& spec,
                const metadata::graphSpec& graph, const metadata::pipeIndex& declared, buildEnv& env,
                const std::map<metadata::pipeRef, metadata::pipeRef>& chainEnds, planBuild& build,
                privateOutputMap privateOutputs)
        : node_(node),
          spec_(spec),
          graph_(graph),
          declared_(declared),
          env_(env),
          chainEnds_(chainEnds),
          build_(build),
          privateOutputs_(std::move(privateOutputs)) {}

    const std::string& id() const noexcept { return node_.id; }
    std::string_view type() const noexcept { return spec_.type; }
    int blockSize() const noexcept { return env_.info.blockSize; }
    double sampleRate() const noexcept { return env_.info.sampleRate; }
    std::uint64_t seed() const noexcept { return rng::seedFor(graph_.seed, node_.id); }
    ioHub& io() noexcept { return env_.io; }

    [[noreturn]] void fail(const std::string& message) const { throw buildError(node_.id, message); }

    double number(std::string_view name) const;
    std::string text(std::string_view name) const;
    // Index of the parameter's choice in its spec's list.
    int choice(std::string_view name) const;
    std::vector<double> list(std::string_view name) const;
    // A text list parameter; empty when not given (io.md §9.1).
    std::vector<std::string> textList(std::string_view name) const;
    // A static color (§7.7.4): absent when it is `none`; the default when
    // the node gives none. A color bound to a pipe fails: use colorParam.
    std::optional<metadata::rgba> color(std::string_view name) const;
    // Color parameter @p name, static or following a control pipe.
    frameColor colorParam(std::string_view name);
    // Numeric parameter @p name of a video node: static, or following a
    // control pipe or an audio pipe's latest sample.
    frameValue frameParam(std::string_view name);
    // The buffer that buffer parameter @p name names, shared with the plan.
    std::shared_ptr<sampleBuffer> buffer(std::string_view name) const;
    // The declared ID of the buffer that buffer parameter @p name names.
    const std::string& bufferId(std::string_view name) const;
    // The plan's slot for the tables of buffer parameter @p name at table
    // size @p size: empty until the first node that needs them fills it.
    std::shared_ptr<const wavetableSet>& wavetables(std::string_view name, std::size_t size);
    // The frame output @p port: its event pipe and a ring for this node to
    // shape and fill.
    frameOutput frameOut(std::string_view port);
    // The frame input @p port: its event pipe and the ring of the node
    // that writes it, which runs first and has shaped it.
    frameInput frameIn(std::string_view port);
    // Counts @p bytes, which the node allocates for frames or spectral
    // history, against the engine's buffer memory; fails past it (§5.6).
    void reserve(std::size_t bytes);
    // As reserve(), for memory the node can do without: false, counting
    // nothing, past the engine's buffer memory.
    bool tryReserve(std::size_t bytes) noexcept;
    // The image file text parameter @p name names, decoded by the engine's
    // loader (§7.7.6). Fails when it cannot load.
    image loadImage(std::string_view name) const;
    signal param(std::string_view name);
    // The parameter @p name, or the constant @p fallback when the node's
    // type has no such parameter (one class serving several types).
    signal paramOr(std::string_view name, float fallback);

    // True when the node gives parameter @p name, as a value or a pipe.
    bool given(std::string_view name) const { return value(name).has_value(); }

    // The pipe bound to a port; null (or an output without a pipe) when
    // the port is unbound.
    std::shared_ptr<const aPipe> audioIn(std::string_view port);
    audioOutput audioOut(std::string_view port);
    std::shared_ptr<const ePipe> eventIn(std::string_view port);
    std::shared_ptr<ePipe> eventOut(std::string_view port);
    std::vector<std::shared_ptr<ePipe>> eventOuts(std::string_view port);
    controlOutput controlOut(std::string_view port);
    std::shared_ptr<const vPipe> videoIn(std::string_view port);
    std::shared_ptr<vPipe> videoOut(std::string_view port);
    // A constant 0 when the port is unbound.
    signal signalIn(std::string_view port, std::size_t which = 0);
    std::size_t inputCount(std::string_view port) const;
    // Channels declared for the first pipe on input @p port, 1 when it is
    // unbound. A pipe's channels change only when its plan becomes active
    // (primitives.md §6.5), so a node that sizes state by its input asks
    // here, not the pipe or its signal.
    int inputChannels(std::string_view port) const;
    signalOut signalOutput(std::string_view port);

    // Each opens a port of the I/O layer for the node, as the ioHub function
    // of the same name does, with the node as the port's owner where it
    // takes one, and records it so the plan activates it (takePorts).
    std::shared_ptr<midiPort> openMidiIn(std::string device) {
        return kept(env_.io.openMidiIn(id(), std::move(device)));
    }
    std::shared_ptr<midiPort> openMidiOut(std::string device) {
        return kept(env_.io.openMidiOut(std::move(device)));
    }
    std::shared_ptr<oscPort> openOscIn(std::uint16_t port, std::string address) {
        return kept(env_.io.openOscIn(id(), port, std::move(address)));
    }
    std::shared_ptr<oscPort> openOscOut(std::uint16_t port) { return kept(env_.io.openOscOut(port)); }
    std::shared_ptr<serialPort> openSerialIn(std::string device, std::uint32_t baud) {
        return kept(env_.io.openSerialIn(id(), std::move(device), baud));
    }
    std::shared_ptr<serialPort> openSerialOut(std::string device, std::uint32_t baud) {
        return kept(env_.io.openSerialOut(id(), std::move(device), baud));
    }
    std::shared_ptr<webPort> openWebIn(portKind kind, webRequest request) {
        return kept(env_.io.openWebIn(kind, id(), std::move(request)));
    }
    std::shared_ptr<webPort> openWebOut(webRequest request) {
        return kept(env_.io.openWebOut(id(), std::move(request)));
    }
    std::shared_ptr<savePort> openSave(std::string bufferId, std::string file, int channels,
                                       std::size_t frames, double sampleRate) {
        return kept(
            env_.io.openSave(id(), std::move(bufferId), std::move(file), channels, frames, sampleRate));
    }
    // The ports the node opened.
    std::vector<std::shared_ptr<ioPort>> takePorts() { return std::move(ports_); }
    // The private pipes the node bound to its outputs; one it did not bind
    // (an unused port) is not among them.
    privateOutputMap takePrivateOutputs() { return std::move(boundPrivate_); }
    // Records that process() adds to the device's outputs, which orders
    // the node after every other node that does (threads.md §4.1).
    void writeDevice() noexcept { writesDevice_ = true; }
    bool writesDevice() const noexcept { return writesDevice_; }
    // Records that process() reports faults, which orders the node after
    // every other node that does, so the fault queue has one producer at
    // a time (threads.md §4.1, §4.4), and returns what it reports them
    // with.
    [[nodiscard]] faultReporter reportFaults() {
        reportsFaults_ = true;
        return {env_.io, node_.id};
    }
    bool reportsFaults() const noexcept { return reportsFaults_; }

private:
    metadata::optionalRef<metadata::paramValue> value(std::string_view name) const;
    // The index of the declared buffer that buffer parameter @p name names.
    std::int32_t bufferIndex(std::string_view name) const;
    // The buffer memory the plan holds with @p bytes more, in bytes.
    double memoryWith(std::size_t bytes) const noexcept;
    // Numeric parameter @p name as a T built by @p make from its static
    // value, or bound to the audio or control pipe it names (param,
    // frameParam).
    template <typename T, typename Make>
    T numeric(std::string_view name, Make make);
    template <typename P>
    std::shared_ptr<P> bound(bool input, std::string_view port, metadata::pipeKind kind, std::string_view what);
    const metadata::paramSpec& paramSpec(std::string_view name) const;
    // Records @p port so the plan activates it, and returns it.
    template <typename P>
    std::shared_ptr<P> kept(std::shared_ptr<P> port) {
        ports_.push_back(port);
        return port;
    }
    std::shared_ptr<pipe> resolve(metadata::pipeRef ref) const;
    // As resolve(), for an output: the node's private pipe for @p ref, if
    // the builder gave it one.
    std::shared_ptr<pipe> resolveOutput(metadata::pipeRef ref);
    metadata::optionalRef<std::vector<metadata::pipeRef>> refs(bool input, std::string_view port) const;
    // Channels declared for @p ref, clamped to 1 to maxChannels like the
    // pipe itself, so no write can pass a pipe's storage.
    int declaredChannels(metadata::pipeRef ref) const;
    int outputChannels(std::string_view port) const;

    const metadata::nodeSpec& node_;
    const metadata::primitiveSpec& spec_;
    const metadata::graphSpec& graph_;
    const metadata::pipeIndex& declared_;
    buildEnv& env_;
    const std::map<metadata::pipeRef, metadata::pipeRef>& chainEnds_;
    planBuild& build_;
    privateOutputMap privateOutputs_;
    privateOutputMap boundPrivate_;  // those of privateOutputs_ the node bound
    std::vector<std::shared_ptr<ioPort>> ports_;
    bool writesDevice_ = false;
    bool reportsFaults_ = false;
};

// --- Node base ------------------------------------------------------------

// Base of the built-in primitives: output helpers over the fade-aware
// writes of primitive (§9.3).
class nodeBase : public primitive {
public:
    explicit nodeBase(const nodeContext& c) : primitive(c.id(), c.type()) {}

protected:
    using primitive::emit;

    // Writes @p samples to @p channel of @p out; nothing when the output is
    // unbound or has fewer channels.
    void emit(const audioOutput& out, int channel, std::span<const float> samples) noexcept {
        if (out.pipe && channel < out.channels) emit(*out.pipe, channel, samples);
    }
    // Writes @p samples to every channel of @p out.
    void emitAll(const audioOutput& out, std::span<const float> samples) noexcept {
        for (int ch = 0; ch < out.channels; ++ch) emit(out, ch, samples);
    }
    // Sets every channel of @p out to @p value.
    void emitAll(const controlOutput& out, float value) noexcept {
        if (!out.pipe) return;
        for (int ch = 0; ch < out.channels; ++ch) emit(*out.pipe, ch, value);
    }
    // Writes @p samples to @p channel of an audio output, or their last
    // value to @p channel of a control output.
    void emit(const signalOut& out, int channel, std::span<const float> samples) noexcept {
        if (out.audio) {
            emit(*out.audio, channel, samples);
        } else if (out.control && !samples.empty()) {
            emit(*out.control, channel, samples.back());
        }
    }
    // Writes @p samples (or their last value) to every channel of @p out.
    void emitAll(const signalOut& out, std::span<const float> samples) noexcept {
        for (int ch = 0; ch < out.channels; ++ch) emit(out, ch, samples);
    }
};

// --- Factories ----------------------------------------------------------

using factory = std::function<std::shared_ptr<primitive>(nodeContext&)>;

struct factoryEntry {
    std::string_view type;
    factory make;
};

template <typename T>
std::shared_ptr<primitive> make(nodeContext& c) {
    return std::make_shared<T>(c);
}

// One table per source file, a function-local static built at first use,
// so a construction failure is an exception the caller sees rather than
// an abort before main.
std::span<const factoryEntry> oscillatorFactories();
std::span<const factoryEntry> filterFactories();
std::span<const factoryEntry> modulationFactories();
std::span<const factoryEntry> delayFactories();
std::span<const factoryEntry> mathFactories();
std::span<const factoryEntry> eventFactories();
std::span<const factoryEntry> deviceFactories();
std::span<const factoryEntry> imageFactories();
std::span<const factoryEntry> bufferFactories();
std::span<const factoryEntry> spectralFactories();

} // namespace primitives
