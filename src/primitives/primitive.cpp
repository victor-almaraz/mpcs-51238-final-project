// Fade-aware writes shared by every primitive (primitives.md §9.3).

#include <proj/primitives/event.h>
#include <proj/primitives/pipe.h>
#include <proj/primitives/primitive.h>

namespace primitives {

void primitive::emit(aPipe& out, int channel, std::span<const float> samples) noexcept {
    if (gainStart_ == 1.0f && gainEnd_ == 1.0f) {
        out.add(channel, samples);
    } else {
        out.addRamp(channel, samples, gainStart_, gainEnd_);
    }
}

void primitive::emit(cPipe& out, int channel, float value) noexcept {
    if (!retiring_) out.set(channel, value);
}

bool primitive::passes(const event& e) const noexcept { return !retiring_ || e.type == eventType::noteOff; }

bool primitive::emit(ePipe& out, const event& e) noexcept { return passes(e) && out.write(e); }

void primitive::emitDevice(float* out, std::span<const float> samples) noexcept {
    const std::size_t n = samples.size();
    const float step = n > 0 ? (gainEnd_ - gainStart_) / static_cast<float>(n) : 0.0f;
    float g = gainStart_;
    for (std::size_t i = 0; i < n; ++i, g += step) out[i] += samples[i] * g;
}

} // namespace primitives
