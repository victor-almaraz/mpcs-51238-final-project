// Buffers and the pool that adopts them across swaps (samples.md §4.4, §5).

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>

namespace primitives {

sampleBuffer::sampleBuffer(int channels, std::size_t frames)
    : channels_(std::clamp(channels, 1, metadata::maxChannels)),
      frames_(frames),
      samples_(static_cast<std::size_t>(channels_) * frames, 0.0f) {}

std::span<float> sampleBuffer::channel(int c) noexcept {
    return std::span<float>(samples_).subspan(static_cast<std::size_t>(c) * frames_, frames_);
}

std::span<const float> sampleBuffer::channel(int c) const noexcept {
    return std::span<const float>(samples_).subspan(static_cast<std::size_t>(c) * frames_, frames_);
}

void sampleBuffer::fill(const decodedAudio& audio) noexcept {
    const auto fileChannels = static_cast<int>(audio.channels.size());
    if (fileChannels == 0) return;
    const std::size_t n = std::min(frames_, audio.frames());
    const auto copy = [&](int to, int from) {
        const auto& source = audio.channels[static_cast<std::size_t>(from)];
        std::copy_n(source.begin(), std::min(n, source.size()), channel(to).begin());
    };
    if (fileChannels == channels_) {
        for (int c = 0; c < channels_; ++c) copy(c, c);
    } else if (fileChannels == 1) {
        for (int c = 0; c < channels_; ++c) copy(c, 0);  // a mono file fills every channel
    } else if (channels_ == 1) {
        // A mono buffer takes the average of the file's channels.
        const auto out = channel(0);
        const float scale = 1.0f / static_cast<float>(fileChannels);
        for (const auto& source : audio.channels) {
            const std::size_t m = std::min(n, source.size());
            for (std::size_t i = 0; i < m; ++i) out[i] += source[i] * scale;
        }
    } else {
        // The channels both have fill in order; the rest stay silent.
        for (int c = 0; c < std::min(channels_, fileChannels); ++c) copy(c, c);
    }
}

bufferPool::key bufferPool::keyOf(const metadata::bufferDecl& decl) {
    return {.id = decl.id, .channels = decl.channels, .seconds = decl.seconds, .file = decl.file};
}

std::shared_ptr<sampleBuffer> bufferPool::find(const key& k) {
    const std::scoped_lock lock(mutex_);
    const auto it = live_.find(k);
    return it == live_.end() ? nullptr : it->second.lock();
}

void bufferPool::add(const key& k, const std::shared_ptr<sampleBuffer>& buffer) {
    const std::scoped_lock lock(mutex_);
    live_[k] = buffer;
}

std::size_t bufferPool::liveBytes() {
    const std::scoped_lock lock(mutex_);
    std::erase_if(live_, [](const auto& entry) { return entry.second.expired(); });
    std::size_t total = 0;
    for (const auto& [k, weak] : live_) {
        if (const auto b = weak.lock()) total += b->bytes();
    }
    return total;
}

}  // namespace primitives
