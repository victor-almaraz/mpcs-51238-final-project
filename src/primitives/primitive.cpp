// Fade-aware writes shared by every primitive (primitives.md §9.3).

#include <cstddef>
#include <cstdint>
#include <span>

#include <espresso/primitives/event.h>
#include <espresso/primitives/pipe.h>
#include <espresso/primitives/primitive.h>
#include <espresso/primitives/realtime.h>

namespace primitives {

void primitive::emit(aPipe& out, int channel, std::span<const float> samples) noexcept ESPRESSO_NONBLOCKING {
    if (gainStart_ == 1.0f && gainEnd_ == 1.0f) {
        out.add(channel, samples);
    } else {
        out.addRamp(channel, samples, gainStart_, gainEnd_);
    }
}

void primitive::emit(cPipe& out, int channel, float value) noexcept ESPRESSO_NONBLOCKING {
    if (!retiring_) out.set(channel, value);
}

bool primitive::passes(const event& e) const noexcept ESPRESSO_NONBLOCKING {
    return !retiring_ || e.type == eventType::noteOff;
}

bool primitive::emit(ePipe& out, const event& e) noexcept ESPRESSO_NONBLOCKING {
    return passes(e) && out.write(e);
}

bool primitive::forward(ePipe& out, const ePipe& from, const event& e) noexcept ESPRESSO_NONBLOCKING {
    return passes(e) && out.copyFrom(from, e);
}

bool primitive::emitOsc(ePipe& out, std::uint32_t offset,
                        std::span<const std::byte> message) noexcept ESPRESSO_NONBLOCKING {
    return !retiring_ && out.writeOsc(offset, message);
}

void primitive::emitDevice(std::span<float> out,
                           std::span<const float> samples) noexcept ESPRESSO_NONBLOCKING {
    addRamped(out, samples, gainStart_, gainEnd_);
}

} // namespace primitives
