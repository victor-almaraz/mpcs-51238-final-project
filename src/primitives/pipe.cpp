// Pipes and the pipe table (primitives.md §5, §6).

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include <proj/metadata/graph.h>
#include <proj/primitives/event.h>
#include <proj/primitives/pipe.h>
#include <proj/primitives/pipeTable.h>
#include <proj/primitives/primitive.h>
#include <proj/primitives/realtime.h>

namespace primitives {

using metadata::pipeKind;
using metadata::pipeRef;

void addRamped(std::span<float> dst, std::span<const float> src, float gainStart,
               float gainEnd) noexcept PROJ_NONBLOCKING {
    const std::size_t n = std::min(src.size(), dst.size());
    const float step = n > 0 ? (gainEnd - gainStart) / static_cast<float>(n) : 0.0f;
    float g = gainStart;
    for (std::size_t i = 0; i < n; ++i, g += step) dst[i] += src[i] * g;
}

void pipe::configure(int channels, bool feedback) noexcept PROJ_NONBLOCKING {
    channels_ = std::clamp(channels, 1, metadata::maxChannels);
    feedback_ = feedback;
}

// --- aPipe --------------------------------------------------------------

aPipe::aPipe(int blockSize)
    : blockSize_(blockSize),
      numSamples_(blockSize),
      first_(static_cast<std::size_t>(metadata::maxChannels * blockSize), 0.0f),
      second_(first_.size(), 0.0f) {}

void aPipe::configure(int channels, bool feedback) noexcept PROJ_NONBLOCKING {
    const int previous = channels_;
    const bool wasFeedback = feedback_;
    pipe::configure(channels, feedback);
    if (channels_ != previous || feedback_ != wasFeedback) {
        std::ranges::fill(first_, 0.0f);
        std::ranges::fill(second_, 0.0f);
        swapped_ = false;
    }
}

std::span<const float> aPipe::read(int channel) const noexcept PROJ_NONBLOCKING {
    if (channel < 0 || channel >= metadata::maxChannels) return {};
    return channelOf(readBuffer(), channel).first(static_cast<std::size_t>(numSamples_));
}

std::span<float> aPipe::written(int channel) noexcept PROJ_NONBLOCKING {
    if (channel < 0 || channel >= channels_) return {};
    return channelOf(writeBuffer(), channel).first(static_cast<std::size_t>(numSamples_));
}

void aPipe::add(int channel, std::span<const float> src, float gain) noexcept PROJ_NONBLOCKING {
    const auto dst = written(channel);
    const std::size_t n = std::min(src.size(), dst.size());
    for (std::size_t i = 0; i < n; ++i) dst[i] += src[i] * gain;
}

void aPipe::addRamp(int channel, std::span<const float> src, float gainStart,
                    float gainEnd) noexcept PROJ_NONBLOCKING {
    addRamped(written(channel), src, gainStart, gainEnd);
}

void aPipe::beginBlock(int numSamples) noexcept PROJ_NONBLOCKING {
    numSamples_ = std::clamp(numSamples, 0, blockSize_);
    // A feedback pipe's next reader may read a longer block than this one
    // writes, so its whole buffer is cleared.
    const auto clear = static_cast<std::size_t>(feedback_ ? blockSize_ : numSamples_);
    for (int c = 0; c < channels_; ++c) {
        const auto dst = channelOf(writeBuffer(), c).first(clear);
        std::ranges::fill(dst, 0.0f);
    }
}

void aPipe::endBlock() noexcept PROJ_NONBLOCKING {
    if (feedback_) swapped_ = !swapped_;
}

// --- cPipe --------------------------------------------------------------

void cPipe::configure(int channels, bool feedback) noexcept PROJ_NONBLOCKING {
    const int previous = channels_;
    const bool wasFeedback = feedback_;
    pipe::configure(channels, feedback);
    // Readers see front_ in both modes; a channel that becomes active takes
    // channel 0's value, which a reader of the narrower pipe saw on it.
    const float first = front_[0].load(std::memory_order_relaxed);
    for (auto c = static_cast<std::size_t>(previous); std::cmp_less(c, channels_); ++c) {
        front_[c].store(first, std::memory_order_relaxed);
        back_[c].store(first, std::memory_order_relaxed);
    }
    // The half a feedback pipe publishes at the block end starts with the
    // current values, so a block without writes publishes no stale value.
    if (feedback_ != wasFeedback) {
        for (std::size_t c = 0; c < front_.size(); ++c) {
            back_[c].store(front_[c].load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
    }
}

void cPipe::set(int channel, float v) noexcept PROJ_NONBLOCKING {
    auto& slots = feedback_ ? back_ : front_;
    slots[clampChannel(channel)].store(v, std::memory_order_relaxed);
}

void cPipe::setAll(float v) noexcept PROJ_NONBLOCKING {
    for (int c = 0; c < metadata::maxChannels; ++c) set(c, v);
}

void cPipe::endBlock() noexcept PROJ_NONBLOCKING {
    if (!feedback_) return;
    for (auto c = std::size_t{0}; std::cmp_less(c, channels_); ++c) {
        front_[c].store(back_[c].load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
}

// --- ePipe --------------------------------------------------------------

ePipe::ePipe(std::size_t capacity, std::size_t poolBytes, counter overflowCounter)
    : events_(capacity), pool_(poolBytes), overflows_(std::move(overflowCounter)) {}

void ePipe::overflow() noexcept PROJ_NONBLOCKING { overflows_->fetch_add(1, std::memory_order_relaxed); }

bool ePipe::write(const event& e) noexcept PROJ_NONBLOCKING {
    if (count_ == events_.size()) {
        overflow();
        return false;
    }
    // Insert after every event with an offset not greater than e's, so
    // equal offsets keep writer order (§5.4.2).
    std::size_t at = count_;
    while (at > 0 && events_[at - 1].sampleOffset > e.sampleOffset) {
        events_[at] = events_[at - 1];
        --at;
    }
    events_[at] = e;
    ++count_;
    return true;
}

std::optional<poolRef> ePipe::store(std::span<const std::byte> bytes) noexcept PROJ_NONBLOCKING {
    if (bytes.size() > pool_.size() - poolUsed_) return std::nullopt;
    const poolRef ref{.offset = static_cast<std::uint32_t>(poolUsed_),
                      .length = static_cast<std::uint32_t>(bytes.size())};
    std::ranges::copy(bytes, pool_.begin() + static_cast<std::ptrdiff_t>(poolUsed_));
    poolUsed_ += bytes.size();
    return ref;
}

bool ePipe::writeOsc(std::uint32_t offset, std::span<const std::byte> message) noexcept PROJ_NONBLOCKING {
    if (count_ == events_.size()) {
        overflow();
        return false;
    }
    const auto ref = store(message);
    if (!ref) {
        overflow();
        return false;
    }
    return write(oscEvent(offset, *ref));
}

std::span<const std::byte> ePipe::data(poolRef ref) const noexcept PROJ_NONBLOCKING {
    if (ref.offset > poolUsed_ || ref.length > poolUsed_ - ref.offset) return {};
    return std::span(pool_).subspan(ref.offset, ref.length);
}

bool ePipe::copyFrom(const ePipe& from, const event& e) noexcept PROJ_NONBLOCKING {
    if (e.type != eventType::osc) return write(e);
    return writeOsc(e.sampleOffset, from.data(oscData(e)));
}

void ePipe::beginBlock() noexcept PROJ_NONBLOCKING {
    count_ = 0;
    poolUsed_ = 0;
}

// --- pipeTable ----------------------------------------------------------

pipeTable::pipeTable(const capacity& limits, const prepareInfo& info) {
    for (int i = 0; i < limits.audio; ++i) audio_.push_back(std::make_shared<aPipe>(info.blockSize));
    for (int i = 0; i < std::max(limits.control, 1); ++i) control_.push_back(std::make_shared<cPipe>());
    for (int i = 0; i < limits.event; ++i) {
        event_.push_back(std::make_shared<ePipe>(static_cast<std::size_t>(limits.eventsPerPipe),
                                                 static_cast<std::size_t>(limits.poolBytes), overflows_));
    }
    for (int i = 0; i < limits.video; ++i)
        video_.push_back(std::make_shared<vPipe>(info.frameWidth, info.frameHeight));
    control_[metadata::tempoPipe]->setAll(120.0f);
}

bool pipeTable::contains(pipeRef ref) const noexcept PROJ_NONBLOCKING {
    if (ref.index < 0) return false;
    const auto i = static_cast<std::size_t>(ref.index);
    switch (ref.kind) {
    case pipeKind::audio: return i < audio_.size();
    case pipeKind::control: return i < control_.size();
    case pipeKind::event: return i < event_.size();
    case pipeKind::video: return i < video_.size();
    }
    return false;
}

std::shared_ptr<pipe> pipeTable::find(pipeRef ref) {
    if (!contains(ref)) return nullptr;
    const auto i = static_cast<std::size_t>(ref.index);
    switch (ref.kind) {
    case pipeKind::audio: return audio_[i];
    case pipeKind::control: return control_[i];
    case pipeKind::event: return event_[i];
    case pipeKind::video: return video_[i];
    }
    return nullptr;
}

void pipeTable::configure(const metadata::pipeDecl& decl) noexcept PROJ_NONBLOCKING {
    if (contains(decl.pipe)) {
        const auto i = static_cast<std::size_t>(decl.pipe.index);
        switch (decl.pipe.kind) {
        case pipeKind::audio: audio_[i]->configure(decl.channels, decl.feedback); break;
        case pipeKind::control: control_[i]->configure(decl.channels, decl.feedback); break;
        case pipeKind::event: event_[i]->configure(decl.channels, decl.feedback); break;
        case pipeKind::video: video_[i]->configure(decl.channels, decl.feedback); break;
        }
    }
}

void pipeTable::beginBlock(std::span<const pipeRef> used, int numSamples) noexcept PROJ_NONBLOCKING {
    for (const auto& r : used) {
        if (r.kind == pipeKind::audio) {
            audio(r.index).beginBlock(numSamples);
        } else if (r.kind == pipeKind::event) {
            events(r.index).beginBlock();
        }
    }
}

void pipeTable::endBlock(std::span<const pipeRef> used) noexcept PROJ_NONBLOCKING {
    for (const auto& r : used) {
        if (r.kind == pipeKind::audio) {
            audio(r.index).endBlock();
        } else if (r.kind == pipeKind::control) {
            control(r.index).endBlock();
        }
    }
}

} // namespace primitives
