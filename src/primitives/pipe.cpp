// Pipes and the pipe table (primitives.md §5, §6).

#include <proj/primitives/pipeTable.h>

#include <algorithm>

namespace primitives {

using metadata::pipeKind;
using metadata::pipeRef;

void pipe::configure(int channels, bool feedback) noexcept {
    channels_ = std::clamp(channels, 1, metadata::maxChannels);
    feedback_ = feedback;
}

// --- aPipe --------------------------------------------------------------

aPipe::aPipe(int blockSize)
    : blockSize_(blockSize),
      numSamples_(blockSize),
      first_(static_cast<std::size_t>(metadata::maxChannels * blockSize), 0.0f),
      second_(first_.size(), 0.0f),
      read_(first_.data()),
      write_(first_.data()) {}

void aPipe::configure(int channels, bool feedback) noexcept {
    if (feedback != feedback_) {
        std::fill(first_.begin(), first_.end(), 0.0f);
        std::fill(second_.begin(), second_.end(), 0.0f);
        read_ = first_.data();
        write_ = feedback ? second_.data() : first_.data();
    }
    pipe::configure(channels, feedback);
}

std::span<const float> aPipe::read(int channel) const noexcept {
    return {channelData(read_, channel), static_cast<std::size_t>(numSamples_)};
}

void aPipe::add(int channel, std::span<const float> src, float gain) noexcept {
    float* dst = channelData(write_, channel);
    const std::size_t n = std::min(src.size(), static_cast<std::size_t>(numSamples_));
    for (std::size_t i = 0; i < n; ++i) dst[i] += src[i] * gain;
}

void aPipe::addRamp(int channel, std::span<const float> src, float gainStart, float gainEnd) noexcept {
    float* dst = channelData(write_, channel);
    const std::size_t n = std::min(src.size(), static_cast<std::size_t>(numSamples_));
    const float step = n > 0 ? (gainEnd - gainStart) / static_cast<float>(n) : 0.0f;
    float g = gainStart;
    for (std::size_t i = 0; i < n; ++i, g += step) dst[i] += src[i] * g;
}

void aPipe::beginBlock(int numSamples) noexcept {
    numSamples_ = std::clamp(numSamples, 0, blockSize_);
    for (int c = 0; c < channels_; ++c) {
        float* dst = channelData(write_, c);
        std::fill(dst, dst + numSamples_, 0.0f);
    }
}

void aPipe::endBlock() noexcept {
    if (feedback_) std::swap(read_, write_);
}

// --- cPipe --------------------------------------------------------------

cPipe::cPipe() noexcept {
    for (auto& v : front_) v.store(0.0f, std::memory_order_relaxed);
    for (auto& v : back_) v.store(0.0f, std::memory_order_relaxed);
}

void cPipe::set(int channel, float v) noexcept {
    auto& slots = feedback_ ? back_ : front_;
    slots[clampChannel(channel)].store(v, std::memory_order_relaxed);
}

void cPipe::setAll(float v) noexcept {
    for (int c = 0; c < metadata::maxChannels; ++c) set(c, v);
}

void cPipe::endBlock() noexcept {
    if (!feedback_) return;
    for (int c = 0; c < channels_; ++c) {
        front_[static_cast<std::size_t>(c)].store(back_[static_cast<std::size_t>(c)].load(std::memory_order_relaxed),
                                                  std::memory_order_relaxed);
    }
}

// --- ePipe --------------------------------------------------------------

ePipe::ePipe(std::size_t capacity, std::size_t poolBytes, std::atomic<std::uint64_t>* overflowCounter)
    : events_(capacity), pool_(poolBytes), overflows_(overflowCounter) {}

void ePipe::overflow() noexcept {
    if (overflows_) overflows_->fetch_add(1, std::memory_order_relaxed);
}

bool ePipe::write(const event& e) noexcept {
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

std::optional<poolRef> ePipe::store(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() > pool_.size() - poolUsed_) return std::nullopt;
    const poolRef ref{static_cast<std::uint32_t>(poolUsed_), static_cast<std::uint32_t>(bytes.size())};
    std::copy(bytes.begin(), bytes.end(), pool_.begin() + static_cast<std::ptrdiff_t>(poolUsed_));
    poolUsed_ += bytes.size();
    return ref;
}

bool ePipe::writeOsc(std::uint32_t offset, std::span<const std::byte> message) noexcept {
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

std::span<const std::byte> ePipe::data(poolRef ref) const noexcept {
    if (ref.offset > poolUsed_ || ref.length > poolUsed_ - ref.offset) return {};
    return {pool_.data() + ref.offset, ref.length};
}

bool ePipe::copyFrom(const ePipe& from, const event& e) noexcept {
    if (e.type != eventType::osc) return write(e);
    return writeOsc(e.sampleOffset, from.data(oscData(e)));
}

void ePipe::beginBlock() noexcept {
    count_ = 0;
    poolUsed_ = 0;
}

// --- pipeTable ----------------------------------------------------------

pipeTable::pipeTable(const capacity& limits, const prepareInfo& info) : limits_(limits) {
    for (int i = 0; i < limits.audio; ++i) audio_.push_back(std::make_unique<aPipe>(info.blockSize));
    for (int i = 0; i < std::max(limits.control, 1); ++i) control_.push_back(std::make_unique<cPipe>());
    for (int i = 0; i < limits.event; ++i) {
        event_.push_back(std::make_unique<ePipe>(static_cast<std::size_t>(limits.eventsPerPipe),
                                                 static_cast<std::size_t>(limits.poolBytes), &overflows_));
    }
    control_[metadata::tempoPipe]->setAll(120.0f);
}

bool pipeTable::contains(pipeRef ref) const noexcept {
    if (ref.index < 0) return false;
    const auto i = static_cast<std::size_t>(ref.index);
    switch (ref.kind) {
    case pipeKind::audio: return i < audio_.size();
    case pipeKind::control: return i < control_.size();
    case pipeKind::event: return i < event_.size();
    case pipeKind::video: return false;
    }
    return false;
}

pipe* pipeTable::find(pipeRef ref) noexcept {
    if (!contains(ref)) return nullptr;
    const auto i = static_cast<std::size_t>(ref.index);
    switch (ref.kind) {
    case pipeKind::audio: return audio_[i].get();
    case pipeKind::control: return control_[i].get();
    case pipeKind::event: return event_[i].get();
    case pipeKind::video: return nullptr;
    }
    return nullptr;
}

void pipeTable::configure(const metadata::pipeDecl& decl) noexcept {
    if (pipe* p = find(decl.pipe)) p->configure(decl.channels, decl.feedback);
}

void pipeTable::beginBlock(std::span<const pipeRef> used, int numSamples) noexcept {
    for (const auto& r : used) {
        if (r.kind == pipeKind::audio) {
            audio(r.index).beginBlock(numSamples);
        } else if (r.kind == pipeKind::event) {
            events(r.index).beginBlock();
        }
    }
}

void pipeTable::endBlock(std::span<const pipeRef> used) noexcept {
    for (const auto& r : used) {
        if (r.kind == pipeKind::audio) {
            audio(r.index).endBlock();
        } else if (r.kind == pipeKind::control) {
            control(r.index).endBlock();
        }
    }
}

} // namespace primitives
