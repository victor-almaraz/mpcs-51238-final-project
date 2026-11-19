// Spectral primitives: fft analyzes a signal into frames and ifft
// resynthesizes them by overlap-add (samples.md §10).

#include "spectral.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <espresso/primitives/buffer.h>
#include <espresso/primitives/event.h>
#include <espresso/primitives/primitive.h>
#include <espresso/primitives/random.h>
#include <espresso/primitives/realtime.h>

#include "node.h"

#include <pffft/pffft.h>

namespace primitives {

// --- frameRing ----------------------------------------------------------------

namespace {

std::uint32_t nextRingId() noexcept {
    static std::atomic<std::uint32_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

frameRing::frameRing() : id_(nextRingId()) {}

void frameRing::allocate(int channels, int size, int hop, int slots) {
    channels_ = channels;
    size_ = size;
    hop_ = hop;
    slots_ = slots;
    data_.assign(static_cast<std::size_t>(slots) * static_cast<std::size_t>(channels) *
                     static_cast<std::size_t>(bins()),
                 bin{});
    // No frame is current until it is stamped: sequence numbers start at 0,
    // and every slot starts at a number no frame has.
    sequences_.assign(static_cast<std::size_t>(slots), UINT64_MAX);
}

bool frameRing::holds(const frameRef& where) const noexcept ESPRESSO_NONBLOCKING {
    return where.ring == id_ && std::cmp_less(where.slot, slots_) && sequences_[where.slot] == where.sequence;
}

std::span<const bin> frameRing::frame(std::uint32_t slot, int channel) const noexcept ESPRESSO_NONBLOCKING {
    const auto n = static_cast<std::size_t>(bins());
    return std::span<const bin>(data_).subspan(
        ((static_cast<std::size_t>(slot) * static_cast<std::size_t>(channels_)) +
         static_cast<std::size_t>(channel)) *
            n,
        n);
}

std::span<bin> frameRing::frame(std::uint32_t slot, int channel) noexcept ESPRESSO_NONBLOCKING {
    const auto n = static_cast<std::size_t>(bins());
    return std::span<bin>(data_).subspan(
        ((static_cast<std::size_t>(slot) * static_cast<std::size_t>(channels_)) +
         static_cast<std::size_t>(channel)) *
            n,
        n);
}

// --- realFft ------------------------------------------------------------------

void realFft::setupDeleter::operator()(PFFFT_Setup* s) const noexcept { pffft_destroy_setup(s); }
void realFft::alignedDeleter::operator()(float* p) const noexcept { pffft_aligned_free(p); }

realFft::aligned realFft::allocate(int floats) {
    // PFFFT's buffers are SIMD-aligned; it allocates them.
    aligned p(static_cast<float*>(pffft_aligned_malloc(static_cast<std::size_t>(floats) * sizeof(float))));
    if (!p) throw std::bad_alloc();
    std::fill_n(p.get(), floats, 0.0f);
    return p;
}

realFft::realFft(int size)
    : size_(size),
      setup_(pffft_new_setup(size, PFFFT_REAL)),
      time_(allocate(size)),
      freq_(allocate(size)),
      work_(allocate(size)) {
    if (!setup_) throw std::invalid_argument("no FFT of size " + std::to_string(size));
}

realFft::~realFft() = default;

void realFft::forward(std::span<bin> bins) noexcept ESPRESSO_NONBLOCKING {
    ESPRESSO_ASSUME_NONBLOCKING_BEGIN  // PFFFT transforms in place in its preallocated buffers
    pffft_transform_ordered(setup_.get(), time_.get(), freq_.get(), work_.get(), PFFFT_FORWARD);
    ESPRESSO_ASSUME_NONBLOCKING_END
    const std::span<const float> f(freq_.get(), static_cast<std::size_t>(size_));
    const auto half = static_cast<std::size_t>(size_ / 2);
    // Ordered: DC and Nyquist, both real, share the first pair.
    bins[0] = {f[0], 0.0f};
    bins[half] = {f[1], 0.0f};
    for (std::size_t k = 1; k < half; ++k) bins[k] = {f[2 * k], f[(2 * k) + 1]};
}

void realFft::inverse(std::span<const bin> bins) noexcept ESPRESSO_NONBLOCKING {
    const std::span<float> f(freq_.get(), static_cast<std::size_t>(size_));
    const auto half = static_cast<std::size_t>(size_ / 2);
    f[0] = bins[0].real();
    f[1] = bins[half].real();
    for (std::size_t k = 1; k < half; ++k) {
        f[2 * k] = bins[k].real();
        f[(2 * k) + 1] = bins[k].imag();
    }
    ESPRESSO_ASSUME_NONBLOCKING_BEGIN  // as forward()
    pffft_transform_ordered(setup_.get(), freq_.get(), time_.get(), work_.get(), PFFFT_BACKWARD);
    ESPRESSO_ASSUME_NONBLOCKING_END
}

std::vector<float> spectralWindow(int size, int overlap) {
    std::vector<float> w(static_cast<std::size_t>(size));
    for (std::size_t n = 0; n < w.size(); ++n) {
        const double x = static_cast<double>(n) / size;
        w[n] = static_cast<float>(overlap == 4 ? 0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * x))
                                               : std::sin(std::numbers::pi * x));
    }
    return w;
}

namespace {

// --- fft (§10.1, §10.2) ---------------------------------------------------------

class fft final : public nodeBase {
public:
    explicit fft(nodeContext& c)
        : nodeBase(c),
          in_(c.signalIn("in")),
          out_(c.frameOut("out")),
          size_(static_cast<int>(c.number("size"))),
          hop_(size_ / static_cast<int>(c.number("overlap"))),
          channels_(c.inputChannels("in")),
          window_(spectralWindow(size_, static_cast<int>(c.number("overlap")))),
          history_(static_cast<std::size_t>(channels_) * static_cast<std::size_t>(size_), 0.0f),
          transform_(size_) {
        const int slots = ringSlots(c.blockSize(), hop_);
        c.reserve(static_cast<std::size_t>(slots) * static_cast<std::size_t>(channels_) *
                  static_cast<std::size_t>(size_ / 2 + 1) * sizeof(bin));
        out_.ring->allocate(channels_, size_, hop_, slots);
    }

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        auto& o = static_cast<fft&>(outgoing);
        // The history is finite: process() writes non-finite input as 0.
        if (o.size_ != size_ || o.hop_ != hop_ || o.channels_ != channels_) return;
        std::ranges::copy(o.history_, history_.begin());
        write_ = o.write_;
        sinceHop_ = o.sinceHop_;
        sequence_ = o.sequence_;
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto count = static_cast<std::size_t>(n);
        const auto size = static_cast<std::size_t>(size_);
        std::array<std::span<const float>, maxChannels> x{};
        for (int c = 0; c < channels_; ++c) x[static_cast<std::size_t>(c)] = in_.block(c, n);
        frameRing& ring = *out_.ring;
        for (std::size_t i = 0; i < count; ++i) {
            for (std::size_t c = 0; std::cmp_less(c, channels_); ++c) {
                // A non-finite input would stay in every frame it reaches.
                const float v = x[c][i];
                history_[(c * size) + write_] = std::isfinite(v) ? v : 0.0f;
            }
            write_ = (write_ + 1) % size;
            if (++sinceHop_ < hop_) continue;
            sinceHop_ = 0;
            // The last size samples, oldest first, windowed.
            const std::uint32_t slot = ring.slotFor(sequence_);
            const auto t = transform_.time();
            for (int c = 0; c < channels_; ++c) {
                const auto h =
                    std::span<const float>(history_).subspan(static_cast<std::size_t>(c) * size, size);
                for (std::size_t k = 0; k < size; ++k) t[k] = h[(write_ + k) % size] * window_[k];
                transform_.forward(ring.frame(slot, c));
            }
            ring.stamp(slot, sequence_);
            if (out_.events) {
                emit(*out_.events, frameEvent(static_cast<std::uint32_t>(i),
                                              {.ring = ring.id(), .slot = slot, .sequence = sequence_}));
            }
            ++sequence_;
        }
    }

private:
    signal in_;
    frameOutput out_;
    int size_;
    int hop_;
    int channels_;
    std::vector<float> window_;
    std::vector<float> history_;  // size samples a channel, circular
    realFft transform_;
    std::size_t write_ = 0;
    int sinceHop_ = 0;
    std::uint64_t sequence_ = 0;
};

// --- ifft (§10.1, §10.2) --------------------------------------------------------

class ifft final : public nodeBase {
public:
    explicit ifft(nodeContext& c)
        : nodeBase(c),
          in_(c.frameIn("in")),
          out_(c.audioOut("out")),
          size_(in_.ring->size()),
          channels_(in_.ring->channels()),
          window_(spectralWindow(size_, size_ / in_.ring->hop())),
          scale_(static_cast<float>(1.0 / (size_ * overlapGain(size_ / in_.ring->hop())))),
          sum_(static_cast<std::size_t>(channels_) * static_cast<std::size_t>(size_), 0.0f),
          transform_(size_),
          outputs_(static_cast<std::size_t>(out_.channels), blockBuffer(c.blockSize())) {}

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        auto& o = static_cast<ifft&>(outgoing);
        if (o.size_ != size_ || o.channels_ != channels_ ||
            !std::ranges::all_of(o.sum_, [](float v) { return std::isfinite(v); })) {
            return;
        }
        std::ranges::copy(o.sum_, sum_.begin());
        read_ = o.read_;
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto count = static_cast<std::size_t>(n);
        const auto size = static_cast<std::size_t>(size_);
        const auto events = in_.events ? in_.events->events() : std::span<const event>{};
        const frameRing& ring = *in_.ring;
        std::array<float, maxChannels> now{};
        std::size_t next = 0;
        for (std::size_t i = 0; i < count; ++i) {
            // The sum's oldest sample is this output sample; its slot then
            // starts the window of samples to come.
            for (std::size_t c = 0; std::cmp_less(c, channels_); ++c) {
                float& v = sum_[(c * size) + read_];
                now[c] = v;
                v = 0.0f;
            }
            for (int o = 0; o < out_.channels; ++o) {
                outputs_[static_cast<std::size_t>(o)][i] = mapChannel(
                    o, out_.channels, channels_, [&](int c) { return now[static_cast<std::size_t>(c)]; });
            }
            read_ = (read_ + 1) % size;
            // A frame completed at this sample adds from the next one on, so
            // an input sample comes out exactly size samples later.
            for (; next < events.size() && events[next].sampleOffset <= i; ++next) add(events[next], ring);
        }
        for (int o = 0; o < out_.channels; ++o)
            emit(out_, o, outputs_[static_cast<std::size_t>(o)].first(count));
    }

private:
    // Adds the frame event @p e names, inverse transformed and windowed,
    // to the sum from the next output sample on.
    void add(const event& e, const frameRing& ring) noexcept ESPRESSO_NONBLOCKING {
        if (e.type != eventType::frame) return;
        const frameRef where = frameOf(e);
        if (!ring.holds(where)) return;  // another ring's, or overwritten
        const auto size = static_cast<std::size_t>(size_);
        const auto t = transform_.time();
        for (int c = 0; c < channels_; ++c) {
            transform_.inverse(ring.frame(where.slot, c));
            const auto s = std::span<float>(sum_).subspan(static_cast<std::size_t>(c) * size, size);
            for (std::size_t k = 0; k < size; ++k) s[(read_ + k) % size] += t[k] * window_[k] * scale_;
        }
    }

    frameInput in_;
    audioOutput out_;
    int size_;
    int channels_;
    std::vector<float> window_;
    float scale_;
    std::vector<float> sum_;  // size samples a channel, circular, from read_
    realFft transform_;
    std::vector<blockBuffer> outputs_;  // one per output channel
    std::size_t read_ = 0;
};

// --- Spectral processors (§11) ---------------------------------------------------

// @p v, or 0 when it is not finite: a processor's output feeds overlap-add
// sums and feedback histories that would keep a NaN.
bin finite(bin v) noexcept ESPRESSO_NONBLOCKING {
    return std::isfinite(v.real()) && std::isfinite(v.imag()) ? v : bin{};
}

// A processor of frames: each frame its input holds becomes a new frame
// of the same shape in its own ring, never written in place, so one frame
// pipe can feed several processors (§10.3). A derived class transforms
// one frame's channels at a time.
class spectralProcessor : public nodeBase {
public:
    explicit spectralProcessor(nodeContext& c) : nodeBase(c), in_(c.frameIn("in")), out_(c.frameOut("out")) {
        const frameRing& r = *in_.ring;
        const int slots = ringSlots(c.blockSize(), r.hop());
        c.reserve(static_cast<std::size_t>(slots) * static_cast<std::size_t>(r.channels()) *
                  static_cast<std::size_t>(r.bins()) * sizeof(bin));
        out_.ring->allocate(r.channels(), r.size(), r.hop(), slots);
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        beginBlock(ctx);
        const auto events = in_.events ? in_.events->events() : std::span<const event>{};
        const frameRing& in = *in_.ring;
        frameRing& out = *out_.ring;
        for (const event& e : events) {
            if (e.type != eventType::frame) continue;
            const frameRef where = frameOf(e);
            if (!in.holds(where)) continue;
            const std::uint32_t slot = out.slotFor(sequence_);
            beginFrame(e.sampleOffset);
            for (int c = 0; c < in.channels(); ++c) {
                const auto y = out.frame(slot, c);
                transform(c, in.frame(where.slot, c), y);
                for (bin& v : y) v = finite(v);
            }
            out.stamp(slot, sequence_);
            if (out_.events) {
                emit(*out_.events,
                     frameEvent(e.sampleOffset, {.ring = out.id(), .slot = slot, .sequence = sequence_}));
            }
            ++sequence_;
        }
    }

protected:
    const frameRing& input() const noexcept { return *in_.ring; }
    // Once a block, before its frames: read block-rate parameters.
    virtual void beginBlock(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING = 0;
    // Before each frame, completed at @p offset in the block.
    virtual void beginFrame(std::uint32_t offset) noexcept ESPRESSO_NONBLOCKING = 0;
    // Channel @p channel of a frame: @p in to @p out, which do not overlap.
    virtual void transform(int channel, std::span<const bin> in,
                           std::span<bin> out) noexcept ESPRESSO_NONBLOCKING = 0;

private:
    frameInput in_;
    frameOutput out_;
    std::uint64_t sequence_ = 0;
};

// The frames a history holds, for a delay of up to @p seconds at @p hop
// samples a frame.
std::size_t historyFrames(double seconds, double sampleRate, int hop) {
    return static_cast<std::size_t>(std::ceil(seconds * sampleRate / hop)) + 1;
}

// pvDelay (§11.1, §11.2): each bin delayed by its own whole number of hops.
class pvDelay final : public spectralProcessor {
public:
    enum class shape : std::uint8_t { flat, rising, falling, random };

    explicit pvDelay(nodeContext& c)
        : spectralProcessor(c),
          redraw_(c.eventIn("redraw")),
          time_(c.param("time")),
          spread_(c.param("spread")),
          feedback_(c.param("feedback")),
          mix_(c.param("mix")),
          shape_(static_cast<shape>(c.choice("shape"))),
          framesPerSecond_(c.sampleRate() / input().hop()),
          depth_(historyFrames(c.number("max-time"), c.sampleRate(), input().hop())),
          bins_(static_cast<std::size_t>(input().bins())),
          random_(c.seed()),
          draws_(bins_) {
        const auto values = depth_ * static_cast<std::size_t>(input().channels()) * bins_;
        c.reserve(values * sizeof(bin));
        history_.assign(values, bin{});
        for (float& d : draws_) d = random_.uniform();
    }

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        auto& o = static_cast<pvDelay&>(outgoing);
        if (o.history_.size() != history_.size() || o.bins_ != bins_) return;
        const bool finiteHistory = std::ranges::all_of(
            o.history_, [](const bin& v) { return std::isfinite(v.real()) && std::isfinite(v.imag()); });
        if (finiteHistory) {
            std::ranges::copy(o.history_, history_.begin());
            write_ = o.write_;
        }
        std::ranges::copy(o.draws_, draws_.begin());
        random_ = o.random_;
    }

private:
    void beginBlock(const blockContext&) noexcept ESPRESSO_NONBLOCKING override {
        // fmin and fmax pass no NaN.
        time_s_ = std::fmin(std::fmax(static_cast<double>(time_.value(0)), 0.0), 3600.0);
        spread_s_ = std::fmin(std::fmax(spread_.value(0), 0.0f), 1.0f);
        feedback_s_ = std::fmin(std::fmax(feedback_.value(0), -0.99f), 0.99f);
        mix_s_ = std::fmin(std::fmax(mix_.value(0), 0.0f), 1.0f);
        redraws_ = redraw_ ? redraw_->events() : std::span<const event>{};
        nextRedraw_ = 0;
    }

    void beginFrame(std::uint32_t offset) noexcept ESPRESSO_NONBLOCKING override {
        // Redraw events up to this frame draw new random delays.
        bool redraw = false;
        for (; nextRedraw_ < redraws_.size() && redraws_[nextRedraw_].sampleOffset <= offset; ++nextRedraw_)
            redraw = true;
        if (redraw) {
            for (float& d : draws_) d = random_.uniform();
        }
        frame_ = write_;
        write_ = (write_ + 1) % depth_;
    }

    void transform(int channel, std::span<const bin> in,
                   std::span<bin> out) noexcept ESPRESSO_NONBLOCKING override {
        const std::size_t last = bins_ - 1;
        const auto base = static_cast<std::size_t>(channel) * bins_;
        for (std::size_t k = 0; k < bins_; ++k) {
            const double x = last == 0 ? 0.0 : static_cast<double>(k) / static_cast<double>(last);
            double s = 1.0;
            switch (shape_) {
            case shape::flat: s = 1.0; break;
            case shape::rising: s = x; break;
            case shape::falling: s = 1.0 - x; break;
            case shape::random: s = draws_[k]; break;
            }
            const double seconds = time_s_ * ((1.0 - spread_s_) + (spread_s_ * s));
            const auto hops =
                std::min(static_cast<std::size_t>(std::llround(seconds * framesPerSecond_)), depth_ - 1);
            bin& slot = history_[(frame_ * stride()) + base + k];
            const bin delayed =
                hops == 0 ? in[k] : history_[(((frame_ + depth_ - hops) % depth_) * stride()) + base + k];
            slot = hops == 0 ? in[k] : finite(in[k] + (feedback_s_ * delayed));
            out[k] = ((1.0f - mix_s_) * in[k]) + (mix_s_ * delayed);
        }
    }

    std::size_t stride() const noexcept { return static_cast<std::size_t>(input().channels()) * bins_; }

    std::shared_ptr<const ePipe> redraw_;
    signal time_;
    signal spread_;
    signal feedback_;
    signal mix_;
    shape shape_ = shape::flat;
    double framesPerSecond_;
    std::size_t depth_;  // frames of history
    std::size_t bins_;
    rng random_;
    std::vector<float> draws_;  // the random shape's value per bin
    std::vector<bin> history_;  // depth_ frames, each channel by channel
    std::size_t write_ = 0;
    std::size_t frame_ = 0;  // the frame being written
    double time_s_ = 0.0;
    float spread_s_ = 1.0f;
    float feedback_s_ = 0.0f;
    float mix_s_ = 1.0f;
    std::span<const event> redraws_;
    std::size_t nextRedraw_ = 0;
};

// pvFreeze (§11.3): a held spectrum, its phases advancing at each bin's
// measured frequency.
class pvFreeze final : public spectralProcessor {
public:
    explicit pvFreeze(nodeContext& c)
        : spectralProcessor(c),
          hold_(c.param("hold")),
          fadeStep_(c.number("fade") > 0.0 ? input().hop() / (c.number("fade") * c.sampleRate()) : 1.0),
          bins_(static_cast<std::size_t>(input().bins())),
          size_(input().size()),
          hop_(input().hop()),
          values_(static_cast<std::size_t>(input().channels()) * bins_) {
        c.reserve(4 * values_ * sizeof(float));
        previous_.assign(values_, 0.0f);
        magnitude_.assign(values_, 0.0f);
        advance_.assign(values_, 0.0f);
        phase_.assign(values_, 0.0f);
    }

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        auto& o = static_cast<pvFreeze&>(outgoing);
        if (o.values_ != values_) return;
        const auto finiteAll = [](const std::vector<float>& v) {
            return std::ranges::all_of(v, [](float x) { return std::isfinite(x); });
        };
        if (!finiteAll(o.previous_) || !finiteAll(o.magnitude_) || !finiteAll(o.advance_) ||
            !finiteAll(o.phase_) || !std::isfinite(o.gain_)) {
            return;
        }
        std::ranges::copy(o.previous_, previous_.begin());
        std::ranges::copy(o.magnitude_, magnitude_.begin());
        std::ranges::copy(o.advance_, advance_.begin());
        std::ranges::copy(o.phase_, phase_.begin());
        gain_ = o.gain_;
        holding_ = o.holding_;
    }

private:
    void beginBlock(const blockContext&) noexcept ESPRESSO_NONBLOCKING override {
        const bool hold = hold_.value(0) > 0.5f;
        capture_ = hold && !holding_;  // a rising hold captures the next frame
        holding_ = hold;
    }

    void beginFrame(std::uint32_t) noexcept ESPRESSO_NONBLOCKING override {
        gain_ = holding_ ? std::fmin(gain_ + fadeStep_, 1.0) : std::fmax(gain_ - fadeStep_, 0.0);
    }

    void transform(int channel, std::span<const bin> in,
                   std::span<bin> out) noexcept ESPRESSO_NONBLOCKING override {
        const auto base = static_cast<std::size_t>(channel) * bins_;
        constexpr double twoPi = 2.0 * std::numbers::pi;
        for (std::size_t k = 0; k < bins_; ++k) {
            const std::size_t at = base + k;
            const float phase = std::arg(in[k]);
            if (capture_) {
                // The instantaneous frequency, as the phase advance over a hop:
                // the bin's own advance plus the deviation, wrapped to ±π.
                const double expected = twoPi * static_cast<double>(k) * hop_ / size_;
                double deviation = static_cast<double>(phase - previous_[at]) - expected;
                deviation -= twoPi * std::round(deviation / twoPi);
                magnitude_[at] = std::abs(in[k]);
                advance_[at] = static_cast<float>(expected + deviation);
                phase_[at] = phase;
            } else if (gain_ > 0.0) {
                phase_[at] =
                    static_cast<float>(std::remainder(static_cast<double>(phase_[at]) + advance_[at], twoPi));
            }
            previous_[at] = phase;
            const auto g = static_cast<float>(gain_);
            out[k] = g > 0.0f ? ((1.0f - g) * in[k]) + (g * std::polar(magnitude_[at], phase_[at])) : in[k];
        }
        if (channel + 1 == input().channels()) capture_ = false;
    }

    signal hold_;
    double fadeStep_;
    std::size_t bins_;
    int size_;
    int hop_;
    std::size_t values_;
    std::vector<float> previous_;   // each bin's input phase, last frame
    std::vector<float> magnitude_;  // the held spectrum
    std::vector<float> advance_;    // its phase advance per hop
    std::vector<float> phase_;      // its phase now
    double gain_ = 0.0;             // 0 passes the input; 1 is the held spectrum
    bool holding_ = false;
    bool capture_ = false;
};

// pvBlur (§11.4): each bin's magnitude smoothed over frames, its phase the input's.
class pvBlur final : public spectralProcessor {
public:
    explicit pvBlur(nodeContext& c)
        : spectralProcessor(c),
          amount_(c.param("amount")),
          bins_(static_cast<std::size_t>(input().bins())),
          smoothed_(static_cast<std::size_t>(input().channels()) * bins_, 0.0f) {
        c.reserve(smoothed_.size() * sizeof(float));
    }

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        auto& o = static_cast<pvBlur&>(outgoing);
        if (o.smoothed_.size() == smoothed_.size() &&
            std::ranges::all_of(o.smoothed_, [](float x) { return std::isfinite(x); })) {
            std::ranges::copy(o.smoothed_, smoothed_.begin());
        }
    }

private:
    void beginBlock(const blockContext&) noexcept ESPRESSO_NONBLOCKING override {
        amount_s_ = std::fmin(std::fmax(amount_.value(0), 0.0f), 1.0f);
    }
    void beginFrame(std::uint32_t) noexcept ESPRESSO_NONBLOCKING override {}

    void transform(int channel, std::span<const bin> in,
                   std::span<bin> out) noexcept ESPRESSO_NONBLOCKING override {
        const auto base = static_cast<std::size_t>(channel) * bins_;
        for (std::size_t k = 0; k < bins_; ++k) {
            float& m = smoothed_[base + k];
            const float magnitude = std::abs(in[k]);
            m = (amount_s_ * m) + ((1.0f - amount_s_) * magnitude);
            out[k] = magnitude > 0.0f ? in[k] * (m / magnitude) : bin{};
        }
    }

    signal amount_;
    std::size_t bins_;
    std::vector<float> smoothed_;
    float amount_s_ = 0.0f;
};

}  // namespace

std::span<const factoryEntry> spectralFactories() {
    static const auto factories = std::to_array<factoryEntry>({
        {.type = "fft", .make = make<fft>},
        {.type = "ifft", .make = make<ifft>},
        {.type = "pvDelay", .make = make<pvDelay>},
        {.type = "pvFreeze", .make = make<pvFreeze>},
        {.type = "pvBlur", .make = make<pvBlur>},
    });
    return factories;
}

}  // namespace primitives
