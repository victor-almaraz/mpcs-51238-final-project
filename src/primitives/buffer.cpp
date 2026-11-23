// Buffers and the pool that adopts them across swaps (samples.md §4.4, §5).

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>

namespace primitives {

sampleBuffer::sampleBuffer(int channels, std::size_t frames, double sampleRate)
    : channels_(std::clamp(channels, 1, metadata::maxChannels)),
      frames_(frames),
      sampleRate_(sampleRate),
      samples_(static_cast<std::size_t>(channels_) * frames, 0.0f) {}

std::size_t sampleBuffer::levelBytes() const noexcept {
    const auto bytesOf = [](const std::shared_ptr<const bufferLevels>& set) {
        std::size_t total = 0;
        if (set) {
            for (const auto& l : *set) total += l.bytes();
        }
        return total;
    };
    // Both rules' sets share one set of lines.
    const auto& lines = levels_[0].lines ? levels_[0].lines : levels_[1].lines;
    return bytesOf(lines) + bytesOf(levels_[0].ends) + bytesOf(levels_[1].ends);
}

void sampleBuffer::markWritten() noexcept {
    written_ = true;
    levels_ = {};  // the plans that read them hold their own
}

void sampleBuffer::setLevels(bufferEdge edge, levelSet levels) noexcept {
    levels_[static_cast<std::size_t>(edge)] = std::move(levels);
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
    return {.id = decl.id,
            .channels = decl.channels,
            .seconds = decl.seconds,
            .file = decl.file,
            .resample = decl.resample};
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
        if (const auto b = weak.lock()) total += b->bytes() + b->levelBytes();
    }
    return total;
}

}  // namespace primitives
