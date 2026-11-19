// Audio files for the server: decoding, resampling to the engine's rate,
// and a cache of decoded files (samples.md §4). Has no JUCE dependency, so
// server_tests and offline renders use the same code as a live server.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <espresso/primitives/buffer.h>
#include <espresso/primitives/io.h>

namespace server {

/// Decodes the audio file at @p path, at its own sample rate: WAV and AIFF
/// (8-, 16-, 24-, and 32-bit integer, 32- and 64-bit float), FLAC, MP3, and
/// Ogg Vorbis, recognized by their contents. Returns nothing, with @p error
/// set, when the file cannot be read or decoded.
std::optional<primitives::decodedAudio> decodeAudioFile(const std::filesystem::path& path,
                                                        std::string& error);

/// Decodes audio file contents @p bytes, as decodeAudioFile does.
std::optional<primitives::decodedAudio> decodeAudio(std::span<const std::uint8_t> bytes, std::string& error);

/// Kaiser-windowed sinc resampling, 32 zero crossings either side of the
/// center, cut off at half the lower rate (samples.md §4.3).
class resampler {
public:
    /// A resampler from @p from to @p to hertz, both positive.
    resampler(double from, double to);

    /// @p in, resampled. Its length is the input's times to / from,
    /// rounded; at equal rates, the input itself.
    std::vector<float> run(std::span<const float> in) const;

    /// Resamples @p channels, all of one length, in place, with each output
    /// sample's kernel taps computed once for every channel; at equal rates
    /// they are left as they are.
    void run(std::vector<std::vector<float>>& channels) const;

private:
    double from_;
    double to_;
};

/// Decoded files, resampled, kept for reuse (samples.md §4.5): the 16 most
/// recently used, keyed by resolved path, modification time, size, and
/// sample rate, so an edited file decodes again. Safe to call from any
/// thread.
class sampleCache {
public:
    /// Most files kept.
    static constexpr std::size_t capacity = 16;

    /// The file at @p path, decoded and resampled to @p sampleRate, or at
    /// its own rate when @p sampleRate is 0; null, with @p error set, when
    /// it cannot be loaded.
    std::shared_ptr<const primitives::decodedAudio> load(const std::filesystem::path& path, double sampleRate,
                                                         std::string& error);

    /// Files decoded so far, counting each miss: for tests.
    std::size_t decodes() const;

private:
    struct key {
        std::string path;
        std::filesystem::file_time_type modified;
        std::uintmax_t size = 0;
        double sampleRate = 0.0;
        friend bool operator==(const key&, const key&) = default;
    };
    struct entry {
        key k;
        std::shared_ptr<const primitives::decodedAudio> audio;
    };

    mutable std::mutex mutex_;
    std::list<entry> recent_;  // most recently used first
    std::size_t decodes_ = 0;
};

/// A loader for the engine that resolves relative paths against @p root,
/// the project root (samples.md §4.2), through @p cache.
primitives::sampleLoader sampleLoaderFor(std::filesystem::path root, std::shared_ptr<sampleCache> cache);

/// One save of a buffer to a file (samples.md §18): the node that saved
/// it, the buffer's ID, the file, and why it failed; no error when it was
/// written.
struct saveOutcome {
    std::string node;
    std::string bufferId;
    std::string file;
    std::string error;

    /// The failure, as `node N: cannot save buffer B to F: error`.
    std::string message() const {
        return "node " + node + ": cannot save buffer " + bufferId + " to " + file + ": " + error;
    }
};

/// Control thread: writes each complete snapshot of @p hub's save ports to
/// its file under @p root, the project root, as 32-bit float WAV, making
/// the directories on its path. A file that is absolute or leaves the root
/// is not written.
std::vector<saveOutcome> writeSaves(primitives::ioHub& hub, const std::filesystem::path& root);

}  // namespace server
