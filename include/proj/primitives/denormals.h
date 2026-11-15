/// @file
/// Flush-to-zero and denormals-are-zero for the audio thread (AGENTS.md,
/// real-time rules). Decaying filter and delay tails reach denormal
/// values, which are slow on most CPUs; the guard treats them as zero.

#pragma once

#include <cstdint>

#include <proj/primitives/realtime.h>

#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace primitives {

/// Enables flush-to-zero and denormals-are-zero for its scope and restores
/// the previous floating-point mode on exit.
class denormalGuard {
public:
    denormalGuard() noexcept PROJ_NONBLOCKING {
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
        saved_ = _mm_getcsr();
        _mm_setcsr(static_cast<unsigned>(saved_) | 0x8040u);  // FTZ (bit 15) and DAZ (bit 6)
#elif defined(__aarch64__)
        asm volatile("mrs %0, fpcr" : "=r"(saved_));
        asm volatile("msr fpcr, %0" : : "r"(saved_ | (std::uint64_t{1} << 24)));  // FZ
#endif
    }

    ~denormalGuard() PROJ_NONBLOCKING {
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
        _mm_setcsr(static_cast<unsigned>(saved_));
#elif defined(__aarch64__)
        asm volatile("msr fpcr, %0" : : "r"(saved_));
#endif
    }

    denormalGuard(const denormalGuard&) = delete;
    denormalGuard& operator=(const denormalGuard&) = delete;

private:
    std::uint64_t saved_ = 0;
};

} // namespace primitives
