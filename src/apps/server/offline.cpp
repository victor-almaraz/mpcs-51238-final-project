#include "offline.h"

#include <fstream>
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

std::vector<std::vector<float>> renderOffline(primitives::engine& audio, std::unique_ptr<primitives::plan> plan,
                                              std::size_t frames, int outputs) {
    audio.play(std::move(plan), 0.0, primitives::quantization::none);
    const auto blockSize = static_cast<std::size_t>(audio.info().blockSize);
    std::vector<std::vector<float>> channels(static_cast<std::size_t>(outputs), std::vector<float>(frames, 0.0f));
    std::vector<float*> pointers(channels.size());
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min(blockSize, frames - done);
        for (std::size_t c = 0; c < channels.size(); ++c) pointers[c] = channels[c].data() + done;
        const primitives::deviceBuffers device{nullptr, 0, pointers.data(), outputs};
        audio.process(device, static_cast<int>(n), static_cast<double>(done) / audio.info().sampleRate);
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
    const auto put = [&](auto v) { f.write(reinterpret_cast<const char*>(&v), sizeof v); };
    const auto numChannels = static_cast<std::uint16_t>(channels.size());
    const auto frames = static_cast<std::uint32_t>(channels.empty() ? 0 : channels[0].size());
    const std::uint32_t dataBytes = frames * numChannels * 4u;
    f.write("RIFF", 4);
    put(std::uint32_t{36 + dataBytes});
    f.write("WAVEfmt ", 8);
    put(std::uint32_t{16});
    put(std::uint16_t{3});  // IEEE float
    put(numChannels);
    put(static_cast<std::uint32_t>(sampleRate));
    put(static_cast<std::uint32_t>(sampleRate) * numChannels * 4u);
    put(static_cast<std::uint16_t>(numChannels * 4u));
    put(std::uint16_t{32});
    f.write("data", 4);
    put(dataBytes);
    for (std::uint32_t i = 0; i < frames; ++i) {
        for (const auto& c : channels) put(c[i]);
    }
    return static_cast<bool>(f);
}

} // namespace server
