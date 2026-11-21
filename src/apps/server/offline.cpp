#include "offline.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
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

#include <espresso/primitives/buffer.h>
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
                                              int outputs, double frameRate, const frameSink& onFrame,
                                              const std::function<void()>& afterBlock,
                                              std::span<const std::vector<float>> inputs) {
    audio.play(std::move(plan), 0.0, primitives::quantization::none);
    const auto blockSize = static_cast<std::size_t>(audio.info().blockSize);
    std::vector<std::vector<float>> channels(static_cast<std::size_t>(outputs), std::vector<float>(frames, 0.0f));
    std::vector<std::span<float>> views(channels.size());
    // An input's last frames, and silence past them, are copied into a
    // block of its own.
    std::vector<std::vector<float>> tails(inputs.size(), std::vector<float>(blockSize, 0.0f));
    std::vector<std::span<const float>> inViews(inputs.size());
    std::size_t nextFrame = 0;
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min(blockSize, frames - done);
        for (std::size_t c = 0; c < channels.size(); ++c) views[c] = std::span(channels[c]).subspan(done, n);
        for (std::size_t c = 0; c < inputs.size(); ++c) {
            const std::span<const float> in = inputs[c];
            if (done + n <= in.size()) {
                inViews[c] = in.subspan(done, n);
                continue;
            }
            auto& tail = tails[c];
            std::ranges::fill(tail, 0.0f);
            if (done < in.size()) std::ranges::copy(in.subspan(done), tail.begin());
            inViews[c] = std::span<const float>(tail).first(n);
        }
        audio.process({.inputs = inViews, .outputs = views}, static_cast<int>(n),
                      static_cast<double>(done) / audio.info().sampleRate);
        done += n;
        if (afterBlock) afterBlock();
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

bool writeWav(const std::string& path, std::span<const std::span<const float>> channels, double sampleRate) {
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

bool writeWav(const std::string& path, std::span<const std::vector<float>> channels, double sampleRate) {
    const std::vector<std::span<const float>> spans(channels.begin(), channels.end());
    return writeWav(path, spans, sampleRate);
}

bool writeWav(const std::string& path, const primitives::sampleBuffer& buffer, double sampleRate) {
    std::vector<std::span<const float>> spans;
    spans.reserve(static_cast<std::size_t>(buffer.channels()));
    for (int c = 0; c < buffer.channels(); ++c) spans.push_back(buffer.channel(c));
    return writeWav(path, spans, sampleRate);
}

std::string profileReport(const primitives::plan& plan, double seconds) {
    struct row {
        std::string_view type;
        std::size_t nodes = 0;
        double seconds = 0.0;
    };
    std::vector<row> types;
    double total = 0.0;
    for (std::size_t i = 0; i < plan.nodes.size() && i < plan.seconds.size(); ++i) {
        const auto type = plan.nodes[i]->type();
        auto it = std::ranges::find(types, type, &row::type);
        if (it == types.end()) it = types.insert(types.end(), row{.type = type, .nodes = 0, .seconds = 0.0});
        ++it->nodes;
        it->seconds += plan.seconds[i];
        total += plan.seconds[i];
    }
    std::ranges::sort(types, std::ranges::greater{}, &row::seconds);
    const auto percent = [](double part, double whole) { return whole > 0.0 ? 100.0 * part / whole : 0.0; };
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    out << "profile: " << plan.nodes.size() << " nodes spent " << total * 1000.0 << " ms on "
        << seconds * 1000.0 << " ms of audio: " << percent(total, seconds) << "% of one core\n";
    out << "type                   nodes      ms   share\n";
    for (const auto& r : types) {
        out << std::left << std::setw(20) << r.type << std::right << std::setw(8) << r.nodes << std::setw(8)
            << r.seconds * 1000.0 << std::setw(7) << percent(r.seconds, total) << "%\n";
    }
    std::vector<std::size_t> order(std::min(plan.nodes.size(), plan.seconds.size()));
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    const std::size_t shown = std::min<std::size_t>(order.size(), 10);
    std::ranges::partial_sort(order, order.begin() + static_cast<std::ptrdiff_t>(shown),
                              std::ranges::greater{}, [&](std::size_t i) { return plan.seconds[i]; });
    out << "slowest nodes:\n";
    for (std::size_t k = 0; k < shown; ++k) {
        const std::size_t i = order[k];
        out << "  " << std::setw(7) << plan.seconds[i] * 1000.0 << " ms  " << plan.nodes[i]->type() << "  "
            << plan.nodes[i]->id() << "\n";
    }
    return out.str();
}

} // namespace server
