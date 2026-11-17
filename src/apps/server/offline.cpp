#include "offline.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/primitives/engine.h>
#include <espresso/primitives/plan.h>

namespace server {

std::optional<std::string> readText(const std::string& path) {
    std::stringstream text;
    if (path == "-") {
        text << std::cin.rdbuf();
    } else {
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;
        text << in.rdbuf();
    }
    return text.str();
}

std::vector<std::vector<float>> renderOffline(primitives::engine& audio,
                                              std::shared_ptr<primitives::plan> plan, std::size_t frames,
                                              int outputs, double frameRate, const frameSink& onFrame) {
    audio.play(std::move(plan), 0.0, primitives::quantization::none);
    const auto blockSize = static_cast<std::size_t>(audio.info().blockSize);
    std::vector<std::vector<float>> channels(static_cast<std::size_t>(outputs), std::vector<float>(frames, 0.0f));
    std::vector<std::span<float>> views(channels.size());
    std::size_t nextFrame = 0;
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min(blockSize, frames - done);
        for (std::size_t c = 0; c < channels.size(); ++c) views[c] = std::span(channels[c]).subspan(done, n);
        audio.process({.inputs = {}, .outputs = views}, static_cast<int>(n),
                      static_cast<double>(done) / audio.info().sampleRate);
        done += n;
        if (!onFrame || !(frameRate > 0.0)) continue;
        // Every frame time the audio has now passed, in order: the render
        // thread's work, here on one thread in a fixed order.
        const double reached = static_cast<double>(done) / audio.info().sampleRate;
        if (static_cast<double>(nextFrame) / frameRate >= reached) continue;  // none due yet
        audio.collect();  // the activated plan reaches the render side
        for (; static_cast<double>(nextFrame) / frameRate < reached; ++nextFrame) {
            const double t = static_cast<double>(nextFrame) / frameRate;
            if (const auto f = audio.renderFrame(t)) onFrame(nextFrame, f->get());
        }
    }
    return channels;
}

namespace {

constexpr std::uint64_t maxRiffBytes = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t bytesPerSample = 4;

}  // namespace

std::size_t maxWavFrames(int channels) noexcept {
    const auto perFrame = static_cast<std::uint64_t>(std::max(channels, 1)) * bytesPerSample;
    // The RIFF size counts every byte after its own 8-byte header.
    return static_cast<std::size_t>((maxRiffBytes - (wavHeaderBytes - 8)) / perFrame);
}

// WAVE fields are little-endian; std::bit_cast writes the host's order.
static_assert(std::endian::native == std::endian::little, "writeWav assumes a little-endian host");

bool writeWav(const std::string& path, const std::vector<std::vector<float>>& channels, double sampleRate) {
    const std::size_t frameCount = channels.empty() ? 0 : channels[0].size();
    if (frameCount > maxWavFrames(static_cast<int>(channels.size()))) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const auto put = [&f](auto v) {
        const auto bytes = std::bit_cast<std::array<char, sizeof v>>(v);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };
    const auto tag = [&f](std::string_view fourcc) {
        f.write(fourcc.data(), static_cast<std::streamsize>(fourcc.size()));
    };
    const auto numChannels = static_cast<std::uint16_t>(channels.size());
    const auto frames = static_cast<std::uint32_t>(frameCount);
    const auto rate = static_cast<std::uint32_t>(sampleRate);
    const auto blockAlign = static_cast<std::uint16_t>(numChannels * bytesPerSample);
    const std::uint32_t dataBytes = frames * blockAlign;

    tag("RIFF");
    put(static_cast<std::uint32_t>(wavHeaderBytes - 8 + dataBytes));
    tag("WAVE");
    // Non-PCM formats carry an 18-byte fmt chunk (cbSize) and a fact chunk.
    tag("fmt ");
    put(std::uint32_t{18});
    put(std::uint16_t{3});  // WAVE_FORMAT_IEEE_FLOAT
    put(numChannels);
    put(rate);
    put(static_cast<std::uint32_t>(rate * blockAlign));  // bytes per second
    put(blockAlign);
    put(static_cast<std::uint16_t>(bytesPerSample * 8));
    put(std::uint16_t{0});  // cbSize: no extension
    tag("fact");
    put(std::uint32_t{4});
    put(frames);  // sample frames per channel
    tag("data");
    put(dataBytes);
    for (std::size_t i = 0; i < frameCount && f; ++i) {
        for (const auto& c : channels) put(c[i]);
    }
    f.close();  // flushes; a failed flush or close sets failbit
    return !f.fail();
}

} // namespace server
