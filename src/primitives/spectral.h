// Internal: frame rings, which carry spectral frames between primitives,
// and the real FFT behind fft and ifft (samples.md §10).

#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <espresso/primitives/event.h>
#include <espresso/primitives/realtime.h>

struct PFFFT_Setup;

namespace primitives {

// One complex value per bin.
using bin = std::complex<float>;

// Preallocated spectral frames that one primitive writes and others read
// (samples.md §10.3). The builder makes one, empty, for each frame pipe;
// the pipe's writer shapes it; its readers find frames through the frame
// events on the pipe, which name the ring by its ID. A slot holds a frame
// while its sequence number matches the event's, so a reader of another
// plan's frames, or of a frame since overwritten, finds none.
class frameRing {
public:
    // Takes the next engine-wide ID.
    frameRing();

    // Shapes the ring: @p slots slots of @p channels channels of the bins of
    // a @p size-point transform, one frame each @p hop samples.
    void allocate(int channels, int size, int hop, int slots);
    bool shaped() const noexcept { return slots_ > 0; }

    std::uint32_t id() const noexcept { return id_; }
    int channels() const noexcept { return channels_; }
    int size() const noexcept { return size_; }
    int bins() const noexcept { return size_ / 2 + 1; }
    int hop() const noexcept { return hop_; }

    // True when @p where names a frame this ring holds now.
    bool holds(const frameRef& where) const noexcept ESPRESSO_NONBLOCKING;
    // Channel @p channel of the frame in @p slot.
    std::span<const bin> frame(std::uint32_t slot, int channel) const noexcept ESPRESSO_NONBLOCKING;

    // The slot that frame @p sequence goes in, to be filled and stamped.
    std::uint32_t slotFor(std::uint64_t sequence) const noexcept ESPRESSO_NONBLOCKING {
        return static_cast<std::uint32_t>(sequence % static_cast<std::uint64_t>(slots_));
    }
    std::span<bin> frame(std::uint32_t slot, int channel) noexcept ESPRESSO_NONBLOCKING;
    // Marks @p slot as holding frame @p sequence.
    void stamp(std::uint32_t slot, std::uint64_t sequence) noexcept ESPRESSO_NONBLOCKING {
        sequences_[slot] = sequence;
    }

private:
    std::uint32_t id_;
    int channels_ = 0;
    int size_ = 0;
    int hop_ = 0;
    int slots_ = 0;
    std::vector<bin> data_;  // slot by slot, channel by channel, bin by bin
    std::vector<std::uint64_t> sequences_;
};

// A real FFT of one size, with its own aligned buffers (PFFFT). Transforms
// allocate nothing and are deterministic for a build (samples.md §10.6).
class realFft {
public:
    // A transform of @p size points, a power of two of at least 32.
    explicit realFft(int size);
    ~realFft();
    realFft(const realFft&) = delete;
    realFft& operator=(const realFft&) = delete;
    realFft(realFft&&) = delete;
    realFft& operator=(realFft&&) = delete;

    int size() const noexcept { return size_; }
    // The time samples: the forward transform's input, the inverse's output.
    std::span<float> time() noexcept { return {time_.get(), static_cast<std::size_t>(size_)}; }

    // The bins of time(): size / 2 + 1 of them, DC to Nyquist.
    void forward(std::span<bin> bins) noexcept ESPRESSO_NONBLOCKING;
    // Time samples from @p bins into time(), unscaled: forward then
    // inverse multiplies by size().
    void inverse(std::span<const bin> bins) noexcept ESPRESSO_NONBLOCKING;

private:
    struct setupDeleter {
        void operator()(PFFFT_Setup* s) const noexcept;
    };
    struct alignedDeleter {
        void operator()(float* p) const noexcept;
    };
    // Owns the first float of an aligned block; time() and the transforms
    // view the block through spans of size_ floats.
    using aligned = std::unique_ptr<float, alignedDeleter>;
    static aligned allocate(int floats);

    int size_;
    std::unique_ptr<PFFFT_Setup, setupDeleter> setup_;
    aligned time_;
    aligned freq_;
    aligned work_;
};

// The analysis and synthesis window for @p overlap (samples.md §10.2):
// periodic Hann at 4, periodic sine at 2.
std::vector<float> spectralWindow(int size, int overlap);

// What the window's square overlap-adds to at @p overlap: 1.5 for Hann at
// 4, 1 for sine at 2.
constexpr double overlapGain(int overlap) noexcept { return overlap == 4 ? 1.5 : 1.0; }

// The band-limited tables of a buffer at one table size (samples.md §9.2):
// each table resynthesized once per octave of band limit, version v
// keeping harmonics 1 to (size / 2) >> v, at twice the table's length.
// Made once per plan for each buffer and size, and shared by the
// wavetableOsc nodes that play them.
struct wavetableSet {
    std::size_t tables = 1;
    std::size_t versions = 1;
    std::size_t length = 0;      // samples a version holds: twice the table
    std::vector<float> samples;  // table by table, version by version
};

// Frame slots enough for two blocks of @p blockSize at @p hop, and one more.
constexpr int ringSlots(int blockSize, int hop) noexcept { return (2 * ((blockSize + hop - 1) / hop)) + 1; }

}  // namespace primitives
