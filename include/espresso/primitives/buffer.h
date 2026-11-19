/// @file
/// Buffers: audio memory that a piece declares and its primitives share,
/// kept across plan swaps (`samples.md` §3, §5).

#pragma once

#include <cstddef>
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

namespace primitives {

/// Decoded audio at the engine's sample rate: what a sample loader returns
/// (`samples.md` §3.3).
struct decodedAudio {
    /// One vector of samples per channel, all of one length.
    std::vector<std::vector<float>> channels;

    /// The length, in frames: that of the first channel, or 0.
    std::size_t frames() const noexcept { return channels.empty() ? 0 : channels.front().size(); }
};

/// Decodes the audio file at @p path and resamples it to @p sampleRate.
/// Returns nothing, with @p error set, when it cannot. Runs on the
/// builder thread (`primitives` does no file I/O).
using sampleLoader =
    std::function<std::optional<decodedAudio>(std::string_view path, double sampleRate, std::string& error)>;

/// A block of audio memory of a fixed number of channels and frames,
/// planar and zeroed when made (`samples.md` §2). Allocated by the
/// builder, written and read on the audio thread, freed with the last
/// plan that holds it on the control thread.
class sampleBuffer {
public:
    /// A silent buffer of @p channels channels, 1 to `metadata::maxChannels`,
    /// and @p frames frames.
    sampleBuffer(int channels, std::size_t frames);

    /// Channel count.
    int channels() const noexcept { return channels_; }
    /// Length, in frames.
    std::size_t frames() const noexcept { return frames_; }
    /// Memory held, in bytes.
    std::size_t bytes() const noexcept { return samples_.size() * sizeof(float); }

    /// The samples of channel @p c, from 0.
    std::span<float> channel(int c) noexcept;
    /// The samples of channel @p c, from 0.
    std::span<const float> channel(int c) const noexcept;

    /// Copies @p audio in from the start, mapping its channels to the
    /// buffer's (`samples.md` §4.4). A longer file is cut; a shorter one
    /// leaves the rest silent.
    void fill(const decodedAudio& audio) noexcept;

private:
    int channels_;
    std::size_t frames_;
    std::vector<float> samples_;  // channel 0's frames, then channel 1's
};

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

        /// Ordered by every field.
        friend bool operator<(const key& a, const key& b) {
            return std::tie(a.id, a.channels, a.seconds, a.file) <
                   std::tie(b.id, b.channels, b.seconds, b.file);
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
