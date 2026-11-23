// Spectral primitives: fft analyzes a signal into frames and ifft
// resynthesizes them by overlap-add (samples.md §10).

#include "spectral.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
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
        w[n] = static_cast<float>(overlap == 4 ? 0.5 - (0.5 * std::cos(twoPi * x)) : std::sin(pi * x));
    }
    return w;
}

namespace {

// Shapes @p ring for @p channels channels of @p size-point frames every
// @p hop samples, its memory counted against the engine's bound (§5.6).
void shapeRing(nodeContext& c, frameRing& ring, int channels, int size, int hop) {
    const int slots = ringSlots(c.blockSize(), hop);
    c.reserve(static_cast<std::size_t>(slots) * static_cast<std::size_t>(channels) *
              static_cast<std::size_t>((size / 2) + 1) * sizeof(bin));
    ring.allocate(channels, size, hop, slots);
}

// The samples of @p from, a circular buffer read from @p start, into @p to,
// each times @p window: from[start], from[start + 1], and on round.
void unroll(std::span<const float> from, std::size_t start, std::span<const float> window,
            std::span<float> to) noexcept ESPRESSO_NONBLOCKING {
    const std::size_t first = from.size() - start;
    for (std::size_t k = 0; k < first; ++k) to[k] = from[start + k] * window[k];
    for (std::size_t k = first; k < from.size(); ++k) to[k] = from[k - first] * window[k];
}

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
          window_(spectralWindow(size_, size_ / hop_)),
          history_(static_cast<std::size_t>(channels_) * static_cast<std::size_t>(size_), 0.0f),
          transform_(size_) {
        shapeRing(c, *out_.ring, channels_, size_, hop_);
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
            if (++write_ == size) write_ = 0;
            if (++sinceHop_ < hop_) continue;
            sinceHop_ = 0;
            // The last size samples, oldest first, windowed.
            const std::uint32_t slot = ring.slotFor(sequence_);
            const auto t = transform_.time();
            for (int c = 0; c < channels_; ++c) {
                const auto h =
                    std::span<const float>(history_).subspan(static_cast<std::size_t>(c) * size, size);
                unroll(h, write_, window_, t);
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
          sum_(static_cast<std::size_t>(channels_) * static_cast<std::size_t>(size_), 0.0f),
          transform_(size_),
          outputs_(static_cast<std::size_t>(out_.channels), blockBuffer(c.blockSize())) {
        const int overlap = size_ / in_.ring->hop();
        window_ = spectralWindow(size_, overlap);
        scale_ = static_cast<float>(1.0 / (size_ * overlapGain(overlap)));
    }

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        auto& o = static_cast<ifft&>(outgoing);
        if (o.size_ != size_ || o.channels_ != channels_ || !isFinite(o.sum_)) return;
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
            if (++read_ == size) read_ = 0;
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
            // The sum is circular from read_: two runs, to its end and from its start.
            const std::size_t first = size - read_;
            for (std::size_t k = 0; k < first; ++k) s[read_ + k] += t[k] * window_[k] * scale_;
            for (std::size_t k = first; k < size; ++k) s[k - first] += t[k] * window_[k] * scale_;
        }
    }

    frameInput in_;
    audioOutput out_;
    int size_;
    int channels_;
    std::vector<float> window_;
    float scale_ = 1.0f;      // undoes the transform's gain and the windows' overlap
    std::vector<float> sum_;  // size samples a channel, circular, from read_
    realFft transform_;
    std::vector<blockBuffer> outputs_;  // one per output channel
    std::size_t read_ = 0;
};

// --- Spectral processors (§11) ---------------------------------------------------

// @p v, or 0 when it is not finite: a processor's output feeds overlap-add
// sums and feedback histories that would keep a NaN.
bin finite(bin v) noexcept ESPRESSO_NONBLOCKING { return isFinite(v) ? v : bin{}; }

// A processor of frames: each frame its input holds becomes a new frame
// of the same shape in its own ring, never written in place, so one frame
// pipe can feed several processors (§10.3). A derived class transforms
// one frame's channels at a time.
class spectralProcessor : public nodeBase {
public:
    explicit spectralProcessor(nodeContext& c)
        : nodeBase(c),
          in_(c.frameIn("in")),
          out_(c.frameOut("out")),
          bins_(static_cast<std::size_t>(in_.ring->bins())) {
        const frameRing& r = *in_.ring;
        shapeRing(c, *out_.ring, r.channels(), r.size(), r.hop());
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
    // The bins of a channel of a frame, in and out.
    std::size_t bins() const noexcept { return bins_; }
    // Once a block, before its frames: read block-rate parameters.
    virtual void beginBlock(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING = 0;
    // Before each frame, completed at @p offset in the block.
    virtual void beginFrame(std::uint32_t /*offset*/) noexcept ESPRESSO_NONBLOCKING {}
    // Channel @p channel of a frame: @p in to @p out, which do not overlap.
    virtual void transform(int channel, std::span<const bin> in,
                           std::span<bin> out) noexcept ESPRESSO_NONBLOCKING = 0;

private:
    frameInput in_;
    frameOutput out_;
    std::size_t bins_;  // input().bins(), which the processors loop over
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
          random_(c.seed()),
          draws_(bins()),
          hops_(bins(), 0) {
        const auto values = depth_ * static_cast<std::size_t>(input().channels()) * bins();
        c.reserve(values * sizeof(bin));
        history_.assign(values, bin{});
        for (float& d : draws_) d = random_.uniform();
    }

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        auto& o = static_cast<pvDelay&>(outgoing);
        if (o.history_.size() != history_.size() || o.bins() != bins()) return;
        if (isFinite(o.history_)) {
            std::ranges::copy(o.history_, history_.begin());
            write_ = o.write_;
        }
        std::ranges::copy(o.draws_, draws_.begin());
        random_ = o.random_;
        stale_ = true;  // new draws
    }

private:
    void beginBlock(const blockContext&) noexcept ESPRESSO_NONBLOCKING override {
        const double time = fclamp(static_cast<double>(time_.value(0)), 0.0, 3600.0);
        const float spread = fclamp(spread_.value(0), 0.0f, 1.0f);
        feedback_s_ = fclamp(feedback_.value(0), -0.99f, 0.99f);
        mix_s_ = fclamp(mix_.value(0), 0.0f, 1.0f);
        redraws_ = redraw_ ? redraw_->events() : std::span<const event>{};
        nextRedraw_ = 0;
        if (stale_ || time != time_s_ || spread != spread_s_) {
            time_s_ = time;
            spread_s_ = spread;
            delays();
        }
    }

    // Each bin's delay in hops, from the block's parameters and the draws
    // (§11.2): worked out again when time, spread, or the draws change.
    void delays() noexcept ESPRESSO_NONBLOCKING {
        stale_ = false;
        const std::size_t last = bins() - 1;
        for (std::size_t k = 0; k < bins(); ++k) {
            const double x = last == 0 ? 0.0 : static_cast<double>(k) / static_cast<double>(last);
            double s = 1.0;
            switch (shape_) {
            case shape::flat: s = 1.0; break;
            case shape::rising: s = x; break;
            case shape::falling: s = 1.0 - x; break;
            case shape::random: s = draws_[k]; break;
            }
            const double seconds = time_s_ * ((1.0 - spread_s_) + (spread_s_ * s));
            hops_[k] =
                std::min(static_cast<std::size_t>(std::llround(seconds * framesPerSecond_)), depth_ - 1);
        }
    }

    void beginFrame(std::uint32_t offset) noexcept ESPRESSO_NONBLOCKING override {
        // Redraw events up to this frame draw new random delays.
        bool redraw = false;
        for (; nextRedraw_ < redraws_.size() && redraws_[nextRedraw_].sampleOffset <= offset; ++nextRedraw_)
            redraw = true;
        if (redraw) {
            for (float& d : draws_) d = random_.uniform();
            delays();
        }
        frame_ = write_;
        write_ = (write_ + 1) % depth_;
    }

    void transform(int channel, std::span<const bin> in,
                   std::span<bin> out) noexcept ESPRESSO_NONBLOCKING override {
        const auto base = static_cast<std::size_t>(channel) * bins();
        for (std::size_t k = 0; k < bins(); ++k) {
            const std::size_t hops = hops_[k];
            bin& slot = history_[(frame_ * stride()) + base + k];
            // Below 2 depth_, as frame_ and hops are below depth_: one
            // subtraction wraps it.
            const std::size_t back = frame_ + depth_ - hops;
            const std::size_t from = back < depth_ ? back : back - depth_;
            const bin delayed = hops == 0 ? in[k] : history_[(from * stride()) + base + k];
            slot = hops == 0 ? in[k] : finite(in[k] + (feedback_s_ * delayed));
            out[k] = ((1.0f - mix_s_) * in[k]) + (mix_s_ * delayed);
        }
    }

    std::size_t stride() const noexcept { return static_cast<std::size_t>(input().channels()) * bins(); }

    std::shared_ptr<const ePipe> redraw_;
    signal time_;
    signal spread_;
    signal feedback_;
    signal mix_;
    shape shape_ = shape::flat;
    double framesPerSecond_;
    std::size_t depth_;  // frames of history
    rng random_;
    std::vector<float> draws_;       // the random shape's value per bin
    std::vector<std::size_t> hops_;  // each bin's delay, in hops
    std::vector<bin> history_;  // depth_ frames, each channel by channel
    std::size_t write_ = 0;
    std::size_t frame_ = 0;  // the frame being written
    double time_s_ = 0.0;    // the time and spread hops_ follow
    float spread_s_ = 1.0f;
    bool stale_ = true;  // hops_ lags the draws: before the first block, and after adoption
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
          previous_(static_cast<std::size_t>(input().channels()) * bins()),
          magnitude_(previous_.size(), 0.0f),
          advance_(previous_.size(), 0.0f),
          phase_(previous_.size(), 0.0f) {
        c.reserve(previous_.size() * (sizeof(bin) + (3 * sizeof(float))));
    }

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        auto& o = static_cast<pvFreeze&>(outgoing);
        if (o.previous_.size() != previous_.size() ||
            !allFinite(o.previous_, o.magnitude_, o.advance_, o.phase_, o.gain_)) {
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
        // A rising hold captures the next frame, which may come blocks later.
        if (hold && !holding_) capture_ = true;
        holding_ = hold;
    }

    void beginFrame(std::uint32_t) noexcept ESPRESSO_NONBLOCKING override {
        gain_ = holding_ ? std::fmin(gain_ + fadeStep_, 1.0) : std::fmax(gain_ - fadeStep_, 0.0);
    }

    void transform(int channel, std::span<const bin> in,
                   std::span<bin> out) noexcept ESPRESSO_NONBLOCKING override {
        const auto base = static_cast<std::size_t>(channel) * bins();
        for (std::size_t k = 0; k < bins(); ++k) {
            const std::size_t at = base + k;
            if (capture_) {
                // The instantaneous frequency, as the phase advance over a hop:
                // the bin's own advance plus the deviation, wrapped to ±π.
                const float phase = std::arg(in[k]);
                const double expected = twoPi * static_cast<double>(k) * input().hop() / input().size();
                double deviation = static_cast<double>(phase - std::arg(previous_[at])) - expected;
                deviation -= twoPi * std::round(deviation / twoPi);
                magnitude_[at] = std::abs(in[k]);
                advance_[at] = static_cast<float>(expected + deviation);
                phase_[at] = phase;
            } else if (gain_ > 0.0) {
                phase_[at] =
                    static_cast<float>(std::remainder(static_cast<double>(phase_[at]) + advance_[at], twoPi));
            }
            previous_[at] = in[k];
            const auto g = static_cast<float>(gain_);
            out[k] = g > 0.0f ? ((1.0f - g) * in[k]) + (g * std::polar(magnitude_[at], phase_[at])) : in[k];
        }
        if (channel + 1 == input().channels()) capture_ = false;
    }

    signal hold_;
    double fadeStep_;
    std::vector<bin> previous_;     // each bin of the input's last frame
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
          smoothed_(static_cast<std::size_t>(input().channels()) * bins(), 0.0f) {
        c.reserve(smoothed_.size() * sizeof(float));
    }

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        auto& o = static_cast<pvBlur&>(outgoing);
        if (o.smoothed_.size() == smoothed_.size() && isFinite(o.smoothed_)) {
            std::ranges::copy(o.smoothed_, smoothed_.begin());
        }
    }

private:
    void beginBlock(const blockContext&) noexcept ESPRESSO_NONBLOCKING override {
        amount_s_ = fclamp(amount_.value(0), 0.0f, 1.0f);
    }

    void transform(int channel, std::span<const bin> in,
                   std::span<bin> out) noexcept ESPRESSO_NONBLOCKING override {
        const auto base = static_cast<std::size_t>(channel) * bins();
        for (std::size_t k = 0; k < bins(); ++k) {
            float& m = smoothed_[base + k];
            const float magnitude = std::abs(in[k]);
            m = (amount_s_ * m) + ((1.0f - amount_s_) * magnitude);
            out[k] = magnitude > 0.0f ? in[k] * (m / magnitude) : bin{};
        }
    }

    signal amount_;
    std::vector<float> smoothed_;
    float amount_s_ = 0.0f;
};

// --- wavetableOsc (samples.md §9) ----------------------------------------------

// The versions of a table of @p size: size / 2 harmonics down to 1.
std::size_t versionsFor(std::size_t size) noexcept {
    return static_cast<std::size_t>(std::countr_zero(size / 2)) + 1;
}

// The bytes of the tables of @p frames frames at table size @p size.
std::size_t wavetableBytes(std::size_t frames, std::size_t size) noexcept {
    return (frames / size) * versionsFor(size) * 2 * size * sizeof(float);
}

// The tables of the @p size frames each of @p buffer holds, mixed to mono,
// band limited (wavetableSet).
wavetableSet makeWavetables(const sampleBuffer& buffer, std::size_t size) {
    wavetableSet set{
        .tables = buffer.frames() / size, .versions = versionsFor(size), .length = 2 * size, .samples = {}};
    set.samples.assign(set.tables * set.versions * set.length, 0.0f);
    realFft analysis(static_cast<int>(size));
    realFft synthesis(static_cast<int>(set.length));
    std::vector<bin> spectrum(size / 2 + 1);
    std::vector<bin> band(set.length / 2 + 1);
    const int channels = buffer.channels();
    const float scale = 1.0f / static_cast<float>(size);
    for (std::size_t t = 0; t < set.tables; ++t) {
        const auto time = analysis.time();
        for (std::size_t k = 0; k < size; ++k) {
            time[k] = mapChannel(0, 1, channels, [&](int ch) { return buffer.channel(ch)[(t * size) + k]; });
        }
        analysis.forward(spectrum);
        for (std::size_t v = 0; v < set.versions; ++v) {
            // Harmonics 1 to the version's limit; no DC, no Nyquist.
            const std::size_t limit = (size / 2) >> v;
            std::ranges::fill(band, bin{});
            for (std::size_t k = 1; k <= limit && k < size / 2; ++k) band[k] = spectrum[k];
            synthesis.inverse(band);
            const auto out =
                std::span(set.samples).subspan(((t * set.versions) + v) * set.length, set.length);
            std::ranges::transform(synthesis.time(), out.begin(), [&](float x) { return x * scale; });
        }
    }
    return set;
}

// A band-limited oscillator over the tables of a buffer (wavetableSet),
// which the plan's oscillators of the same buffer and table size share.
// Each sample mixes the two versions next to the band limit its frequency
// allows, so no harmonic reaches half the sample rate and the tone changes
// continuously with the frequency.
class wavetableOsc final : public nodeBase {
public:
    explicit wavetableOsc(nodeContext& c)
        : nodeBase(c),
          freq_(c.param("freq")),
          phase_(c.param("phase")),
          position_(c.param("position")),
          out_(c.audioOut("out")),
          size_(static_cast<std::size_t>(c.number("table-size"))),
          sampleRate_(c.sampleRate()),
          scratch_(c.blockSize()) {
        const auto buffer = c.buffer("buffer");
        if (buffer->frames() < size_) {
            c.fail("the buffer holds " + std::to_string(buffer->frames()) +
                   " frames, less than one table of " + std::to_string(size_));
        }
        auto& shared = c.wavetables("buffer", size_);
        if (!shared) {
            c.reserve(wavetableBytes(buffer->frames(), size_));
            shared = std::make_shared<const wavetableSet>(makeWavetables(*buffer, size_));
        }
        set_ = shared;
        tables_ = set_->tables;
        versions_ = set_->versions;
        length_ = set_->length;
        samples_ = set_->samples;
    }

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        const auto& o = static_cast<const wavetableOsc&>(outgoing);
        if (std::isfinite(o.at_)) at_ = o.at_;
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto count = static_cast<std::size_t>(n);
        const auto f = freq_.block(0, n);
        const auto ph = phase_.block(0, n);
        const auto pos = position_.block(0, n);
        const auto out = scratch_.first(count);
        for (std::size_t i = 0; i < count; ++i) {
            const double freq = std::abs(static_cast<double>(f[i]));
            const double phase = wrapUnit(at_ + static_cast<double>(ph[i]));
            const double place =
                fclamp(static_cast<double>(pos[i]), 0.0, 1.0) * static_cast<double>(tables_ - 1);
            const auto t0 = static_cast<std::size_t>(place);
            const std::size_t t1 = std::min(t0 + 1, tables_ - 1);
            const auto between = static_cast<float>(place - static_cast<double>(t0));
            // The versions depend only on the frequency: worked out again
            // when it changes, so once a block for a steady one.
            if (freq != bandFreq_) {
                band_ = bandFor(freq);
                bandFreq_ = freq;
            }
            float y = 0.0f;
            if (!band_.silent) {
                // A weight of 1 reads one version or table, not two: the other's share is 0.
                const auto read = [&](std::size_t t) {
                    const float low = readTable(t, band_.lo, phase);
                    if (band_.wLo == 1.0f) return low;
                    return (band_.wLo * low) + ((1.0f - band_.wLo) * readTable(t, band_.hi, phase));
                };
                const float first = read(t0);
                y = between == 0.0f ? first : ((1.0f - between) * first) + (between * read(t1));
            }
            out[i] = y;
            at_ = wrapUnit(at_ + (static_cast<double>(f[i]) / sampleRate_));
        }
        emitAll(out_, out);
    }

private:
    // The two versions a frequency plays, and the first's weight.
    struct band {
        bool silent = false;  // at or above half the sample rate
        std::size_t lo = 0;
        std::size_t hi = 0;
        float wLo = 1.0f;
    };

    band bandFor(double freq) const noexcept ESPRESSO_NONBLOCKING {
        const double nyquist = sampleRate_ / 2.0;
        if (!(freq < nyquist)) return {.silent = true, .lo = 0, .hi = 0, .wLo = 1.0f};
        // Version v is safe while v >= x: its top harmonic times freq stays
        // below Nyquist. Between x = v - 1 and v, v fades to v + 1.
        const auto top = static_cast<double>(size_) / 2.0;  // harmonics in the fullest version
        const double x = freq > 0.0 ? std::log2(freq * top / nyquist) : -1.0;
        const auto v = x <= 0.0 ? std::size_t{0} : static_cast<std::size_t>(std::ceil(x));
        return {.silent = false,
                .lo = std::min(v, versions_ - 1),
                .hi = std::min(v + 1, versions_ - 1),
                .wLo = x <= 0.0 ? 1.0f : static_cast<float>(static_cast<double>(v) - x)};
    }

    static double wrapUnit(double x) noexcept ESPRESSO_NONBLOCKING {
        const double w = x - std::floor(x);
        return w >= 0.0 && w < 1.0 ? w : 0.0;  // NaN reads the start
    }

    // Table @p t, version @p v, at @p phase from 0 to 1, by 4-point Hermite.
    float readTable(std::size_t t, std::size_t v, double phase) const noexcept ESPRESSO_NONBLOCKING {
        const auto line = std::span(samples_).subspan(((t * versions_) + v) * length_, length_);
        const double at = phase * static_cast<double>(length_);
        const auto i = static_cast<std::size_t>(at);
        const auto frac = static_cast<float>(at - static_cast<double>(i));
        // length_ is a power of two, so a mask wraps the neighbours.
        const std::size_t mask = length_ - 1;
        return hermite(line[(i + length_ - 1) & mask], line[i & mask], line[(i + 1) & mask],
                       line[(i + 2) & mask], frac);
    }

    signal freq_;
    signal phase_;
    signal position_;
    audioOutput out_;
    std::size_t size_;
    double sampleRate_;
    std::shared_ptr<const wavetableSet> set_;
    // set_'s shape and samples, read by the thread that runs the node.
    std::size_t tables_ = 1;
    std::size_t versions_ = 1;
    std::size_t length_ = 0;
    std::span<const float> samples_;
    blockBuffer scratch_;
    band band_;
    double bandFreq_ = -1.0;  // the frequency band_ is for; none yet
    double at_ = 0.0;  // the phase, 0 to 1
};

}  // namespace

std::span<const factoryEntry> spectralFactories() {
    static const auto factories = std::to_array<factoryEntry>({
        {.type = "fft", .make = make<fft>},
        {.type = "ifft", .make = make<ifft>},
        {.type = "pvDelay", .make = make<pvDelay>},
        {.type = "pvFreeze", .make = make<pvFreeze>},
        {.type = "pvBlur", .make = make<pvBlur>},
        {.type = "wavetableOsc", .make = make<wavetableOsc>},
    });
    return factories;
}

}  // namespace primitives
