/// @file
/// Buffers: audio memory that a piece declares and its primitives share,
/// kept across plan swaps (`samples.md` §3, §5).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/realtime.h>

namespace primitives {

/// Decoded audio: what a sample loader returns (`samples.md` §3.3), at
/// the rate it was asked for or, asked for none, at the file's own.
struct decodedAudio {
    /// One vector of samples per channel, all of one length.
    std::vector<std::vector<float>> channels;
    /// The rate of the samples, in hertz.
    double sampleRate = 0.0;

    /// The length, in frames: that of the first channel, or 0.
    std::size_t frames() const noexcept { return channels.empty() ? 0 : channels.front().size(); }
};

/// Decodes the audio file at @p path and resamples it to @p sampleRate,
/// or, when @p sampleRate is 0, keeps it at its own rate. Returns null, with @p error set, when it cannot.
/// Runs on the builder thread (`primitives` does no file I/O). The result is shared, so a loader may cache
/// it.
using sampleLoader = std::function<std::shared_ptr<const decodedAudio>(
    std::string_view path, double sampleRate, std::string& error)>;

class sampleBuffer;

/// What a read finds past a buffer's ends (`samples.md` §6.6).
enum class bufferEdge : std::uint8_t {
    wrap,    ///< The other end: a ring
    silent,  ///< Silence: a sample played once
};

/// A buffer's levels (`samples.md` §6.6): copies of it at half, a quarter,
/// an eighth, and so on, of its rate, each low-passed below its own
/// Nyquist frequency, which fast reads read instead of the buffer.
using bufferLevels = std::vector<sampleBuffer>;

/// A block of audio memory of a fixed number of channels and frames,
/// planar and zeroed when made (`samples.md` §2). Allocated by the
/// builder, written and read on the audio thread, freed with the last
/// plan that holds it on the control thread.
class sampleBuffer {
public:
    /// A silent buffer of @p channels channels, 1 to `metadata::maxChannels`,
    /// and @p frames frames, holding samples at @p sampleRate hertz.
    sampleBuffer(int channels, std::size_t frames, double sampleRate);

    /// Channel count.
    int channels() const noexcept { return channels_; }
    /// Length, in frames.
    std::size_t frames() const noexcept { return frames_; }
    /// The rate of the samples, in hertz: the engine's, or a file's own
    /// for a buffer that keeps it (`samples.md` §4.5.1).
    double sampleRate() const noexcept { return sampleRate_; }
    /// Memory its samples hold, in bytes.
    std::size_t bytes() const noexcept { return samples_.size() * sizeof(float); }
    /// Builder: memory its levels hold, in bytes.
    std::size_t levelBytes() const noexcept;

    /// The samples of channel @p c, from 0.
    std::span<float> channel(int c) noexcept ESPRESSO_NONBLOCKING;
    /// The samples of channel @p c, from 0.
    std::span<const float> channel(int c) const noexcept ESPRESSO_NONBLOCKING;

    /// Copies @p audio in from the start, mapping its channels to the
    /// buffer's (`samples.md` §4.4). A longer file is cut; a shorter one
    /// leaves the rest silent.
    void fill(const decodedAudio& audio) noexcept;

    /// Builder: marks the buffer as one a writer records into, in the plan
    /// being built or one before, and drops its levels: such a buffer has
    /// none, as they would go stale.
    void markWritten() noexcept;
    /// Builder: takes back markWritten(), for a plan that was refused.
    void unmarkWritten() noexcept { written_ = false; }
    /// Builder: true once markWritten() was called and not taken back.
    bool written() const noexcept { return written_; }
    /// Builder: the buffer's levels for reads that find @p edge past its
    /// ends, or null when none were made.
    const std::shared_ptr<const bufferLevels>& levels(bufferEdge edge) const noexcept {
        return levels_[static_cast<std::size_t>(edge)];
    }
    /// Builder: gives the buffer @p levels for reads that find @p edge past
    /// its ends, made from its contents.
    void setLevels(bufferEdge edge, std::shared_ptr<const bufferLevels> levels) noexcept;

private:
    int channels_;
    std::size_t frames_;
    double sampleRate_;
    std::vector<float> samples_;  // channel 0's frames, then channel 1's
    bool written_ = false;
    std::array<std::shared_ptr<const bufferLevels>, 2> levels_;  // by bufferEdge
};

/// Channel @p to of @p toChannels, taken from a source of @p fromChannels
/// channels whose channel c is @p get(c) (`samples.md` §4.4): with equal
/// counts, channel @p to; from a mono source, its one channel; into a mono
/// destination, the average; otherwise the same channel, or silence past
/// the source's last.
template <typename Get>
float mapChannel(int to, int toChannels, int fromChannels, Get get) noexcept ESPRESSO_NONBLOCKING {
    if (fromChannels == toChannels || fromChannels == 1) return get(fromChannels == 1 ? 0 : to);
    if (toChannels == 1) {
        float sum = 0.0f;
        for (int c = 0; c < fromChannels; ++c) sum += get(c);
        return sum / static_cast<float>(fromChannels);
    }
    return to < fromChannels ? get(to) : 0.0f;
}

/// The bytes a buffer of @p channels channels and @p frames frames holds.
constexpr std::size_t bufferBytes(int channels, std::size_t frames) noexcept {
    return static_cast<std::size_t>(channels) * frames * sizeof(float);
}

/// The buffers alive in an engine, for adoption across swaps (`samples.md`
/// §5.4) and for the memory bound (§5.6). It holds them weakly: a buffer
/// lives as long as a plan holds it. Used by the builder only.
class bufferPool {
public:
    /// Default bound on the memory of live buffers: 1024 MiB.
    static constexpr std::size_t defaultLimit = std::size_t{1024} * 1024 * 1024;

    /// A pool whose live buffers may hold @p limitBytes in all.
    explicit bufferPool(std::size_t limitBytes = defaultLimit) : limit_(limitBytes) {}

    /// The bound, in bytes.
    std::size_t limit() const noexcept { return limit_; }

    /// What makes an incoming buffer the same as a live one (§5.4).
    struct key {
        /// The declaration's ID.
        std::string id;
        /// Channel count.
        int channels = 1;
        /// The declared length, in seconds, if any.
        std::optional<double> seconds;
        /// The declared file, if any.
        std::optional<std::string> file;
        /// Whether the file is resampled.
        bool resample = true;

        /// Ordered by every field.
        friend bool operator<(const key& a, const key& b) {
            return std::tie(a.id, a.channels, a.seconds, a.file, a.resample) <
                   std::tie(b.id, b.channels, b.seconds, b.file, b.resample);
        }
    };

    /// The key of declaration @p decl.
    static key keyOf(const metadata::bufferDecl& decl);

    /// The live buffer with key @p k, if there is one.
    std::shared_ptr<sampleBuffer> find(const key& k);

    /// Records @p buffer under @p k, replacing an expired entry.
    void add(const key& k, const std::shared_ptr<sampleBuffer>& buffer);

    /// Memory held by live buffers, in bytes. Forgets expired ones.
    std::size_t liveBytes();

private:
    std::size_t limit_;
    std::mutex mutex_;
    std::map<key, std::weak_ptr<sampleBuffer>> live_;
};

}  // namespace primitives
