#include "offline.h"

#include <fstream>
#include <iterator>
#include <bit>
#include <array>
#include <iostream>
#include <sstream>

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

std::vector<std::vector<float>> renderOffline(primitives::engine& audio, std::shared_ptr<primitives::plan> plan,
                                              std::size_t frames, int outputs) {
    audio.play(std::move(plan), 0.0, primitives::quantization::none);
    const auto blockSize = static_cast<std::size_t>(audio.info().blockSize);
    std::vector<std::vector<float>> channels(static_cast<std::size_t>(outputs), std::vector<float>(frames, 0.0f));
    std::vector<std::span<float>> views(channels.size());
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min(blockSize, frames - done);
        for (std::size_t c = 0; c < channels.size(); ++c) views[c] = std::span(channels[c]).subspan(done, n);
        audio.process({{}, views}, static_cast<int>(n), static_cast<double>(done) / audio.info().sampleRate);
        done += n;
    }
    return channels;
}

bool fitsWav(std::size_t frames, int channels) noexcept {
    return static_cast<double>(frames) * channels * 4.0 + 36.0 <= 4294967295.0;
}

bool writeWav(const std::string& path, const std::vector<std::vector<float>>& channels, double sampleRate) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    // Little-endian fields, as the format requires (and the host is).
    const auto put = [&](auto v) {
        const auto bytes = std::bit_cast<std::array<char, sizeof v>>(v);
        std::copy(bytes.begin(), bytes.end(), std::ostreambuf_iterator<char>(f));
    };
    const auto numChannels = static_cast<std::uint16_t>(channels.size());
    const auto frames = static_cast<std::uint32_t>(channels.empty() ? 0 : channels[0].size());
    const std::uint32_t dataBytes = frames * numChannels * 4u;
    f << "RIFF";
    put(std::uint32_t{36 + dataBytes});
    f << "WAVEfmt ";
    put(std::uint32_t{16});
    put(std::uint16_t{3});  // IEEE float
    put(numChannels);
    put(static_cast<std::uint32_t>(sampleRate));
    put(static_cast<std::uint32_t>(sampleRate) * numChannels * 4u);
    put(static_cast<std::uint16_t>(numChannels * 4u));
    put(std::uint16_t{32});
    f << "data";
    put(dataBytes);
    for (std::uint32_t i = 0; i < frames; ++i) {
        for (const auto& c : channels) put(c[i]);
    }
    return static_cast<bool>(f);
}

} // namespace server
