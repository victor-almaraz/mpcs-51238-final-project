// Audio files: decoding with dr_libs and stb_vorbis, windowed-sinc
// resampling, and the decoded-file cache (samples.md §4).

#include "samples.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <dr_flac.h>
#include <dr_mp3.h>
#include <dr_wav.h>
#define STB_VORBIS_HEADER_ONLY
#include <espresso/primitives/buffer.h>

#include <stb_vorbis.c>  // NOLINT(bugprone-suspicious-include): stb ships its decoder as one .c file

namespace server {

namespace {

// --- Decoding -----------------------------------------------------------

// Splits @p interleaved, @p channels samples a frame, into one vector per
// channel.
primitives::decodedAudio deinterleave(std::span<const float> interleaved, std::size_t channels) {
    primitives::decodedAudio out;
    const std::size_t frames = channels == 0 ? 0 : interleaved.size() / channels;
    out.channels.assign(channels, std::vector<float>(frames));
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t c = 0; c < channels; ++c) out.channels[c][f] = interleaved[f * channels + c];
    }
    return out;
}

// A decoded file from @p rate, @p channels, and the interleaved samples
// that @p read fills: read(frames, destination) returns the frames read.
// Destinations are spans; a decoder's C call takes their data().
template <typename Read>
std::optional<fileAudio> collect(std::uint32_t rate, std::uint32_t channels, std::uint64_t frames, Read read,
                                 std::string& error) {
    if (rate == 0 || channels == 0) {
        error = "the file names no sample rate or channels";
        return std::nullopt;
    }
    std::vector<float> interleaved(static_cast<std::size_t>(frames) * channels);
    const std::uint64_t got = read(frames, std::span<float>(interleaved));
    interleaved.resize(static_cast<std::size_t>(std::min(got, frames)) * channels);
    return fileAudio{.sampleRate = static_cast<double>(rate), .audio = deinterleave(interleaved, channels)};
}

// WAV, RF64, W64, and AIFF.
std::optional<fileAudio> decodeWav(std::span<const std::uint8_t> bytes, std::string& error) {
    drwav wav{};
    if (drwav_init_memory(&wav, bytes.data(), bytes.size(), nullptr) == DRWAV_FALSE) {
        error = "not a WAV or AIFF file dr_wav can read";
        return std::nullopt;
    }
    // Owns the decoder for this scope.
    const std::unique_ptr<drwav, decltype(&drwav_uninit)> owned(&wav, drwav_uninit);
    return collect(
        wav.sampleRate, wav.channels, wav.totalPCMFrameCount,
        [&](std::uint64_t n, std::span<float> into) {
            return drwav_read_pcm_frames_f32(&wav, n, into.data());
        },
        error);
}

std::optional<fileAudio> decodeFlac(std::span<const std::uint8_t> bytes, std::string& error) {
    const std::unique_ptr<drflac, decltype(&drflac_close)> flac(
        drflac_open_memory(bytes.data(), bytes.size(), nullptr), drflac_close);
    if (!flac) {
        error = "not a FLAC file dr_flac can read";
        return std::nullopt;
    }
    return collect(
        flac->sampleRate, flac->channels, flac->totalPCMFrameCount,
        [&](std::uint64_t n, std::span<float> into) {
            return drflac_read_pcm_frames_f32(flac.get(), n, into.data());
        },
        error);
}

std::optional<fileAudio> decodeMp3(std::span<const std::uint8_t> bytes, std::string& error) {
    drmp3 mp3{};
    if (drmp3_init_memory(&mp3, bytes.data(), bytes.size(), nullptr) == DRMP3_FALSE) {
        error = "not an audio file in a known format (WAV, AIFF, FLAC, MP3, or Ogg Vorbis)";
        return std::nullopt;
    }
    const std::unique_ptr<drmp3, decltype(&drmp3_uninit)> owned(&mp3, drmp3_uninit);
    const std::uint64_t frames = drmp3_get_pcm_frame_count(&mp3);
    return collect(
        mp3.sampleRate, mp3.channels, frames,
        [&](std::uint64_t n, std::span<float> into) {
            return drmp3_read_pcm_frames_f32(&mp3, n, into.data());
        },
        error);
}

std::optional<fileAudio> decodeVorbis(std::span<const std::uint8_t> bytes, std::string& error) {
    if (bytes.size() > static_cast<std::size_t>(INT32_MAX)) {
        error = "the Ogg Vorbis file is too large";
        return std::nullopt;
    }
    int status = 0;
    const std::unique_ptr<stb_vorbis, decltype(&stb_vorbis_close)> vorbis(
        stb_vorbis_open_memory(bytes.data(), static_cast<int>(bytes.size()), &status, nullptr),
        stb_vorbis_close);
    if (!vorbis) {
        error = "not an Ogg Vorbis file stb_vorbis can read";
        return std::nullopt;
    }
    const stb_vorbis_info info = stb_vorbis_get_info(vorbis.get());
    const auto frames = static_cast<std::uint64_t>(stb_vorbis_stream_length_in_samples64(vorbis.get()));
    const auto channels = static_cast<std::uint32_t>(std::max(info.channels, 0));
    return collect(
        info.sample_rate, channels, frames,
        [&](std::uint64_t n, std::span<float> into) {
            // stb_vorbis counts in floats, an int at a time.
            std::uint64_t done = 0;
            while (done < n) {
                const std::uint64_t floats = std::min<std::uint64_t>((n - done) * channels, INT32_MAX / 2);
                const int got = stb_vorbis_get_samples_float_interleaved(
                    vorbis.get(), static_cast<int>(channels), into.subspan(done * channels).data(),
                    static_cast<int>(floats));
                if (got <= 0) break;
                done += static_cast<std::uint64_t>(got);
            }
            return done;
        },
        error);
}

bool startsWith(std::span<const std::uint8_t> bytes, std::string_view magic) {
    return bytes.size() >= magic.size() &&
           std::equal(magic.begin(), magic.end(), bytes.begin(),
                      [](char m, std::uint8_t b) { return static_cast<std::uint8_t>(m) == b; });
}

// --- Resampling ---------------------------------------------------------

constexpr int zeroCrossings = 32;     // either side of the center
constexpr int tableSteps = 1024;      // kernel values per zero crossing, interpolated linearly
constexpr double attenuation = 96.0;  // dB, the Kaiser window's design stopband

// The zeroth-order modified Bessel function of the first kind, by its
// power series; for the Kaiser window.
double besselI0(double x) {
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 64 && term > sum * 1e-17; ++k) {
        const double half = x / (2.0 * k);
        term *= half * half;
        sum += term;
    }
    return sum;
}

// sinc(u) × Kaiser(u / zeroCrossings) at u = i / tableSteps, for u from 0
// to zeroCrossings, and one zero past the end for interpolation.
const std::vector<double>& kernelTable() {
    static const std::vector<double> table = [] {
        const double beta = 0.1102 * (attenuation - 8.7);
        const double norm = besselI0(beta);
        std::vector<double> t(static_cast<std::size_t>(zeroCrossings * tableSteps) + 2, 0.0);
        for (std::size_t i = 0; i + 1 < t.size(); ++i) {
            const double u = static_cast<double>(i) / tableSteps;
            const double r = u / zeroCrossings;
            if (r > 1.0) break;
            const double sinc = u == 0.0 ? 1.0 : std::sin(std::numbers::pi * u) / (std::numbers::pi * u);
            t[i] = sinc * besselI0(beta * std::sqrt(1.0 - (r * r))) / norm;
        }
        return t;
    }();
    return table;
}

double kernel(double u) {
    const auto& t = kernelTable();
    const double at = std::abs(u) * tableSteps;
    const auto i = static_cast<std::size_t>(at);
    if (i + 1 >= t.size()) return 0.0;
    const double frac = at - static_cast<double>(i);
    return t[i] + (frac * (t[i + 1] - t[i]));
}

// --- Files --------------------------------------------------------------

std::optional<std::vector<std::uint8_t>> readFile(const std::filesystem::path& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "no such file, or it cannot be read";
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        error = "it cannot be read";
        return std::nullopt;
    }
    return bytes;
}

}  // namespace

std::optional<fileAudio> decodeAudio(std::span<const std::uint8_t> bytes, std::string& error) {
    if (startsWith(bytes, "RIFF") || startsWith(bytes, "RF64") || startsWith(bytes, "riff") ||
        startsWith(bytes, "FORM")) {
        return decodeWav(bytes, error);
    }
    if (startsWith(bytes, "fLaC")) return decodeFlac(bytes, error);
    if (startsWith(bytes, "OggS")) return decodeVorbis(bytes, error);
    return decodeMp3(bytes, error);  // MP3 has no fixed signature
}

std::optional<fileAudio> decodeAudioFile(const std::filesystem::path& path, std::string& error) {
    const auto bytes = readFile(path, error);
    if (!bytes) return std::nullopt;
    return decodeAudio(*bytes, error);
}

resampler::resampler(double from, double to) : from_(from), to_(to) {}

std::vector<float> resampler::run(std::span<const float> in) const {
    if (from_ == to_ || in.empty()) return {in.begin(), in.end()};
    const double step = from_ / to_;  // input samples per output sample
    // The cutoff, in cycles per input sample: half the lower rate.
    const double cutoff = 0.5 * std::min(1.0, to_ / from_);
    const double scale = 2.0 * cutoff;  // kernel units per input sample, and the gain
    const double reach = zeroCrossings / scale;
    const auto outFrames = static_cast<std::size_t>(std::llround(static_cast<double>(in.size()) / step));
    const auto last = static_cast<std::ptrdiff_t>(in.size()) - 1;
    std::vector<float> out(outFrames);
    for (std::size_t j = 0; j < outFrames; ++j) {
        const double t = static_cast<double>(j) * step;
        const auto first = std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(std::ceil(t - reach)));
        const auto end = std::min<std::ptrdiff_t>(last, static_cast<std::ptrdiff_t>(std::floor(t + reach)));
        double sum = 0.0;
        for (std::ptrdiff_t i = first; i <= end; ++i) {
            sum += in[static_cast<std::size_t>(i)] * kernel((static_cast<double>(i) - t) * scale);
        }
        out[j] = static_cast<float>(sum * scale);
    }
    return out;
}

std::shared_ptr<const primitives::decodedAudio> sampleCache::load(const std::filesystem::path& path,
                                                                  double sampleRate, std::string& error) {
    std::error_code ec;
    const auto resolved = std::filesystem::weakly_canonical(path, ec);
    const auto modified = std::filesystem::last_write_time(resolved, ec);
    const auto size = ec ? std::uintmax_t{0} : std::filesystem::file_size(resolved, ec);
    if (ec) {
        error = "no such file, or it cannot be read";
        return nullptr;
    }
    const key k{.path = resolved.string(), .modified = modified, .size = size, .sampleRate = sampleRate};
    {
        const std::scoped_lock lock(mutex_);
        const auto it = std::ranges::find_if(recent_, [&](const entry& e) { return e.k == k; });
        if (it != recent_.end()) {
            recent_.splice(recent_.begin(), recent_, it);
            return recent_.front().audio;
        }
    }
    // Decoded outside the lock: a long file does not hold up other loads.
    auto decoded = decodeAudioFile(resolved, error);
    if (!decoded) return nullptr;
    const resampler r(decoded->sampleRate, sampleRate);
    for (auto& channel : decoded->audio.channels) channel = r.run(channel);
    auto audio = std::make_shared<const primitives::decodedAudio>(std::move(decoded->audio));
    const std::scoped_lock lock(mutex_);
    ++decodes_;
    recent_.push_front({.k = k, .audio = audio});
    if (recent_.size() > capacity) recent_.pop_back();
    return audio;
}

std::size_t sampleCache::decodes() const {
    const std::scoped_lock lock(mutex_);
    return decodes_;
}

primitives::sampleLoader sampleLoaderFor(std::filesystem::path root, std::shared_ptr<sampleCache> cache) {
    return [root = std::move(root), cache = std::move(cache)](std::string_view path, double sampleRate,
                                                              std::string& error) {
        const std::filesystem::path p(path);
        return cache->load(p.is_absolute() ? p : root / p, sampleRate, error);
    };
}

}  // namespace server
