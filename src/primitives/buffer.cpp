// Buffers and the pool that adopts them across swaps (samples.md §4.4, §5).

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/realtime.h>

namespace primitives {

sampleBuffer::sampleBuffer(int channels, std::size_t frames)
    : channels_(std::clamp(channels, 1, metadata::maxChannels)),
      frames_(frames),
      samples_(static_cast<std::size_t>(channels_) * frames, 0.0f) {}

std::span<float> sampleBuffer::channel(int c) noexcept ESPRESSO_NONBLOCKING {
    return std::span<float>(samples_).subspan(static_cast<std::size_t>(c) * frames_, frames_);
}

std::span<const float> sampleBuffer::channel(int c) const noexcept ESPRESSO_NONBLOCKING {
    return std::span<const float>(samples_).subspan(static_cast<std::size_t>(c) * frames_, frames_);
}

void sampleBuffer::fill(const decodedAudio& audio) noexcept {
    const auto fileChannels = static_cast<int>(audio.channels.size());
    if (fileChannels == 0) return;
    const std::size_t n = std::min(frames_, audio.frames());
    for (int c = 0; c < channels_; ++c) {
        const auto out = channel(c);
        // A channel taken whole, from the same channel or a mono file, copies.
        if (fileChannels == channels_ || fileChannels == 1) {
            const auto& source = audio.channels[static_cast<std::size_t>(fileChannels == 1 ? 0 : c)];
            std::copy_n(source.begin(), std::min(n, source.size()), out.begin());
            continue;
        }
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = mapChannel(c, channels_, fileChannels, [&](int k) {
                const auto& source = audio.channels[static_cast<std::size_t>(k)];
                return i < source.size() ? source[i] : 0.0f;
            });
        }
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
