/// @file
/// The floating-point mode of every thread that runs nodes (AGENTS.md,
/// real-time rules; `threads.md` §6.6): round to nearest, exceptions
/// masked, and flush-to-zero with denormals-are-zero. Decaying filter and
/// delay tails reach denormal values, which are slow on most CPUs; the
/// mode treats them as zero. Every such thread sets the whole control
/// word, so none inherits a rounding mode or a trap from the thread that
/// made it, and every thread computes the same bits.

#pragma once

#include <cstdint>

#include <espresso/primitives/realtime.h>

#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace primitives {

#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
/// The control word of a thread that runs nodes: MXCSR with flush-to-zero
/// (bit 15), every exception masked (bits 7 to 12), round to nearest
/// (bits 13 and 14 clear), denormals-are-zero (bit 6), and no exception
/// flag set.
inline constexpr std::uint64_t nodeFloatControl = 0x9FC0;
/// The bits of the control word that record exceptions that have
/// happened (MXCSR bits 0 to 5), rather than set the mode: arithmetic sets
/// them.
inline constexpr std::uint64_t floatExceptionFlags = 0x3F;
#elif defined(__aarch64__)
/// The control word of a thread that runs nodes: FPCR with flush-to-zero
/// (FZ, bit 24) alone, so round to nearest (RMode, bits 22 and 23, clear)
/// and no exception trap enabled (bits 8 to 12 and 15 clear).
inline constexpr std::uint64_t nodeFloatControl = std::uint64_t{1} << 24U;
/// The bits of the control word that record exceptions: none, since
/// FPSR, not FPCR, holds them.
inline constexpr std::uint64_t floatExceptionFlags = 0;
#else
/// The control word of a thread that runs nodes: none on this processor.
inline constexpr std::uint64_t nodeFloatControl = 0;
/// The bits of the control word that record exceptions: none.
inline constexpr std::uint64_t floatExceptionFlags = 0;
#endif

/// The calling thread's floating-point control word: MXCSR on x86, FPCR on
/// AArch64, 0 elsewhere.
inline std::uint64_t floatControl() noexcept ESPRESSO_NONBLOCKING {
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
    return _mm_getcsr();
#elif defined(__aarch64__)
    std::uint64_t word = 0;
    asm volatile("mrs %0, fpcr" : "=r"(word));
    return word;
#else
    return 0;
#endif
}

/// The calling thread's floating-point mode: its control word without the
/// exception flags (floatExceptionFlags). A thread that runs nodes has
/// mode nodeFloatControl.
inline std::uint64_t floatMode() noexcept ESPRESSO_NONBLOCKING {
    return floatControl() & ~floatExceptionFlags;
}

/// Sets the calling thread's floating-point control word to @p word (see
/// floatControl()).
inline void setFloatControl([[maybe_unused]] std::uint64_t word) noexcept ESPRESSO_NONBLOCKING {
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
    _mm_setcsr(static_cast<unsigned>(word));
#elif defined(__aarch64__)
    asm volatile("msr fpcr, %0" : : "r"(word));
#endif
}

/// Sets the control word of a thread that runs nodes (nodeFloatControl)
/// for its scope and restores the previous one on exit.
class denormalGuard {
public:
    denormalGuard() noexcept ESPRESSO_NONBLOCKING : saved_(floatControl()) {
        setFloatControl(nodeFloatControl);
    }

    ~denormalGuard() ESPRESSO_NONBLOCKING { setFloatControl(saved_); }

    denormalGuard(const denormalGuard&) = delete;
    denormalGuard& operator=(const denormalGuard&) = delete;

private:
    std::uint64_t saved_ = 0;
};

}  // namespace primitives
