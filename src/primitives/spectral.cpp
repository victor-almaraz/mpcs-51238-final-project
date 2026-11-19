// Spectral primitives: fft analyzes a signal into frames and ifft
// resynthesizes them by overlap-add (samples.md §10).

#include "spectral.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
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

}  // namespace

std::span<const factoryEntry> spectralFactories() {
    static const auto factories = std::to_array<factoryEntry>({
        {.type = "fft", .make = make<fft>},
        {.type = "ifft", .make = make<ifft>},
    });
    return factories;
}

}  // namespace primitives
