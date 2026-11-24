// Pipes and the pipe table (primitives.md §5, §6).

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/event.h>
#include <espresso/primitives/pipe.h>
#include <espresso/primitives/pipeTable.h>
#include <espresso/primitives/primitive.h>
#include <espresso/primitives/realtime.h>

namespace primitives {

using metadata::pipeKind;
using metadata::pipeRef;

void addRamped(std::span<float> dst, std::span<const float> src, float gainStart,
               float gainEnd) noexcept ESPRESSO_NONBLOCKING {
    const std::size_t n = std::min(src.size(), dst.size());
    const float step = n > 0 ? (gainEnd - gainStart) / static_cast<float>(n) : 0.0f;
    float g = gainStart;
    for (std::size_t i = 0; i < n; ++i, g += step) dst[i] += src[i] * g;
}

void pipe::configure(int channels, bool feedback) noexcept ESPRESSO_NONBLOCKING {
    channels_ = std::clamp(channels, 1, metadata::maxChannels);
    feedback_ = feedback;
}

// --- aPipe --------------------------------------------------------------

namespace {

// The samples of @p buffers buffers of @p capacity channels of blocks of
// @p blockSize.
std::size_t samplesFor(int blockSize, int capacity, int buffers) {
    return static_cast<std::size_t>(buffers) * static_cast<std::size_t>(capacity) *
           static_cast<std::size_t>(std::max(blockSize, 0));
}

}  // namespace

aPipe::aPipe(int blockSize, int capacity, bool feedback)
    : blockSize_(blockSize),
      numSamples_(blockSize),
      capacity_(std::clamp(capacity, 1, metadata::maxChannels)),
      buffers_(feedback ? 2 : 1),
      owner_(std::make_unique<float[]>(  // NOLINT(modernize-avoid-c-arrays): skill §9
          samplesFor(blockSize, capacity_, buffers_))),
      samples_(owner_.get(), samplesFor(blockSize, capacity_, buffers_)) {}

void aPipe::configure(int channels, bool feedback) noexcept ESPRESSO_NONBLOCKING {
    const int previous = channels_;
    const bool wasFeedback = feedback_;
    pipe::configure(std::min(channels, capacity_), feedback && buffers_ == 2);
    if (channels_ != previous || feedback_ != wasFeedback) {
        std::ranges::fill(samples_, 0.0f);
        swapped_ = false;
    }
}

std::span<const float> aPipe::read(int channel) const noexcept ESPRESSO_NONBLOCKING {
    if (channel < 0 || channel >= capacity_) return {};
    return channelOf(readBuffer(), channel).first(static_cast<std::size_t>(numSamples_));
}

std::span<float> aPipe::written(int channel) noexcept ESPRESSO_NONBLOCKING {
    if (channel < 0 || channel >= channels_) return {};
    // A forwarding pipe keeps its own channel count, as its summing node
    // would add only those channels, and takes the target's buffer and block
    // length, which the target's start set for this block (threads.md
    // §4.2.5). The target is read directly, not through its written(), so
    // that add() and addRamp() inline this function.
    aPipe& into = forwarding_ && target_ ? target_->get() : *this;
    if (channel >= into.channels_) return {};
    return into.channelOf(into.writeBuffer(), channel).first(static_cast<std::size_t>(into.numSamples_));
}

void aPipe::add(int channel, std::span<const float> src, float gain) noexcept ESPRESSO_NONBLOCKING {
    const auto dst = written(channel);
    const std::size_t n = std::min(src.size(), dst.size());
    for (std::size_t i = 0; i < n; ++i) dst[i] += src[i] * gain;
}

void aPipe::addRamp(int channel, std::span<const float> src, float gainStart,
                    float gainEnd) noexcept ESPRESSO_NONBLOCKING {
    addRamped(written(channel), src, gainStart, gainEnd);
}

void aPipe::beginBlock(int numSamples) noexcept ESPRESSO_NONBLOCKING {
    numSamples_ = std::clamp(numSamples, 0, blockSize_);
    // A feedback pipe's next reader may read a longer block than this one
    // writes, so its whole buffer is cleared.
    const auto clear = static_cast<std::size_t>(feedback_ ? blockSize_ : numSamples_);
    for (int c = 0; c < channels_; ++c) {
        const auto dst = channelOf(writeBuffer(), c).first(clear);
        std::ranges::fill(dst, 0.0f);
    }
}

void aPipe::endBlock() noexcept ESPRESSO_NONBLOCKING {
    if (feedback_) swapped_ = !swapped_;
}

// --- cPipe --------------------------------------------------------------

void cPipe::configure(int channels, bool feedback) noexcept ESPRESSO_NONBLOCKING {
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

void cPipe::set(int channel, float v) noexcept ESPRESSO_NONBLOCKING {
    auto& slots = feedback_ ? back_ : front_;
    slots[clampChannel(channel)].store(v, std::memory_order_relaxed);
}

void cPipe::setAll(float v) noexcept ESPRESSO_NONBLOCKING {
    for (int c = 0; c < metadata::maxChannels; ++c) set(c, v);
}

void cPipe::endBlock() noexcept ESPRESSO_NONBLOCKING {
    if (!feedback_) return;
    for (auto c = std::size_t{0}; std::cmp_less(c, channels_); ++c) {
        front_[c].store(back_[c].load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
}

// --- ePipe --------------------------------------------------------------

ePipe::ePipe(std::size_t capacity, std::size_t poolBytes, counter overflowCounter)
    // NOLINTBEGIN(modernize-avoid-c-arrays): skill §9
    : eventStore_(std::make_unique<event[]>(capacity)),
      poolStore_(std::make_unique<std::byte[]>(poolBytes)),
      // NOLINTEND(modernize-avoid-c-arrays)
      events_(eventStore_.get(), capacity),
      pool_(poolStore_.get(), poolBytes),
      overflows_(std::move(overflowCounter)) {}

void ePipe::overflow() noexcept ESPRESSO_NONBLOCKING { overflows_->fetch_add(1, std::memory_order_relaxed); }

bool ePipe::write(const event& e) noexcept ESPRESSO_NONBLOCKING {
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

std::optional<poolRef> ePipe::store(std::span<const std::byte> bytes) noexcept ESPRESSO_NONBLOCKING {
    if (bytes.size() > pool_.size() - poolUsed_) return std::nullopt;
    const poolRef ref{.offset = static_cast<std::uint32_t>(poolUsed_),
                      .length = static_cast<std::uint32_t>(bytes.size())};
    std::ranges::copy(bytes, pool_.begin() + static_cast<std::ptrdiff_t>(poolUsed_));
    poolUsed_ += bytes.size();
    return ref;
}

bool ePipe::writeOsc(std::uint32_t offset, std::span<const std::byte> message) noexcept ESPRESSO_NONBLOCKING {
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

std::span<const std::byte> ePipe::data(poolRef ref) const noexcept ESPRESSO_NONBLOCKING {
    if (ref.offset > poolUsed_ || ref.length > poolUsed_ - ref.offset) return {};
    return std::span<const std::byte>(pool_).subspan(ref.offset, ref.length);
}

bool ePipe::copyFrom(const ePipe& from, const event& e) noexcept ESPRESSO_NONBLOCKING {
    if (e.type != eventType::osc) return write(e);
    return writeOsc(e.sampleOffset, from.data(oscData(e)));
}

void ePipe::beginBlock() noexcept ESPRESSO_NONBLOCKING {
    count_ = 0;
    poolUsed_ = 0;
}

// --- pipeTable ----------------------------------------------------------

pipeTable::pipeTable(const capacity& limits, const prepareInfo& info)
    : audio_(static_cast<std::size_t>(std::max(limits.audio, 0))), info_(info) {
    for (int i = 0; i < std::max(limits.control, 1); ++i) control_.push_back(std::make_shared<cPipe>());
    for (int i = 0; i < limits.event; ++i) {
        event_.push_back(std::make_shared<ePipe>(static_cast<std::size_t>(limits.eventsPerPipe),
                                                 static_cast<std::size_t>(limits.poolBytes), overflows_));
    }
    control_[metadata::tempoPipe]->setAll(120.0f);
}

bool pipeTable::contains(pipeRef ref) const noexcept ESPRESSO_NONBLOCKING {
    if (ref.index < 0) return false;
    const auto i = static_cast<std::size_t>(ref.index);
    switch (ref.kind) {
    case pipeKind::audio: return i < audio_.size();
    case pipeKind::control: return i < control_.size();
    case pipeKind::event: return i < event_.size();
    }
    return false;
}

void pipeTable::make(pipeRef ref) {
    if (!contains(ref)) return;
    const auto i = static_cast<std::size_t>(ref.index);
    if (ref.kind == pipeKind::audio && !audio_[i]) audio_[i] = std::make_shared<aPipe>(info_.blockSize);
}

std::shared_ptr<pipe> pipeTable::find(pipeRef ref) {
    if (!contains(ref)) return nullptr;
    const auto i = static_cast<std::size_t>(ref.index);
    switch (ref.kind) {
    case pipeKind::audio: return audio_[i];
    case pipeKind::control: return control_[i];
    case pipeKind::event: return event_[i];
    }
    return nullptr;
}

void pipeTable::configure(const metadata::pipeDecl& decl) noexcept ESPRESSO_NONBLOCKING {
    if (contains(decl.pipe)) {
        const auto i = static_cast<std::size_t>(decl.pipe.index);
        switch (decl.pipe.kind) {
        case pipeKind::audio: audio_[i]->configure(decl.channels, decl.feedback); break;
        case pipeKind::control: control_[i]->configure(decl.channels, decl.feedback); break;
        case pipeKind::event: event_[i]->configure(decl.channels, decl.feedback); break;
        }
    }
}

void pipeTable::beginBlock(std::span<const pipeRef> pipes, int numSamples) noexcept ESPRESSO_NONBLOCKING {
    for (const auto& r : pipes) {
        if (r.kind == pipeKind::audio) {
            audio(r.index).beginBlock(numSamples);
        } else if (r.kind == pipeKind::event) {
            events(r.index).beginBlock();
        }
    }
}

void pipeTable::endBlock(std::span<const pipeRef> pipes) noexcept ESPRESSO_NONBLOCKING {
    for (const auto& r : pipes) {
        if (r.kind == pipeKind::audio) {
            audio(r.index).endBlock();
        } else if (r.kind == pipeKind::control) {
            control(r.index).endBlock();
        }
    }
}

} // namespace primitives
