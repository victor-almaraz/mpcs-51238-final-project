/// @file
/// The marker for functions that run on the audio thread and the workers
/// (AGENTS.md, real-time rules). Clang's function-effect analysis (-Wfunction-effects)
/// checks a marked function statically, and RealtimeSanitizer
/// (-fsanitize=realtime) aborts when one allocates, frees, locks, or makes
/// a system call at run time; realtimeSanitizerOff exempts one listed
/// call from the sanitizer. realtimeAtomic checks at compile time that an
/// atomic type takes no lock.
///
/// Every primitive includes this header, so it also rejects the builds in
/// which nodes would not compute IEEE single precision step by step, as
/// bit-identical renders need (`threads.md` §1.3, §4.2.1): `-ffast-math`,
/// and float expressions evaluated in a wider type (x87 arithmetic).
/// Contraction into fused multiply-adds is turned off by the build
/// (`-ffp-contract=off`, `cmake/EspressoHelpers.cmake`).

#pragma once

#include <atomic>
#include <cfloat>
#include <cstddef>

#if defined(__FAST_MATH__)
#error "espresso needs IEEE floating point: build without -ffast-math (threads.md §1.3)"
#endif
static_assert(FLT_EVAL_METHOD == 0,
              "espresso evaluates float expressions in float: SSE2 or AArch64 arithmetic, not x87");

/// Marks a function as nonblocking: `void f() noexcept ESPRESSO_NONBLOCKING;`,
/// before `override` or `final` and before the function body. Expands to
/// `[[clang::nonblocking]]` where the compiler knows the attribute, and to
/// nothing elsewhere.
///
/// A macro departs from the modern-cpp rules (no macros for code): an
/// attribute cannot be aliased, and compilers without this one (Apple
/// clang 15, GCC) reject it under -Werror as an unknown attribute. See the
/// skill's section 9.
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::nonblocking)
#define ESPRESSO_NONBLOCKING [[clang::nonblocking]]
#endif
#endif
#ifndef ESPRESSO_NONBLOCKING
#define ESPRESSO_NONBLOCKING
#endif

/// Brackets code in a nonblocking function that Clang's analysis cannot
/// show to be nonblocking but an invariant does: the destructor of a smart
/// pointer that is empty by then, a queue slot the other end emptied, a
/// library call whose storage was reserved beforehand. Each use states
/// its invariant in a comment; RealtimeSanitizer still checks the code at
/// run time. Expands to a diagnostic push that ignores -Wfunction-effects
/// where the compiler has that warning, and to nothing elsewhere.
///
/// A macro, like ESPRESSO_NONBLOCKING, because only `_Pragma` can scope a
/// diagnostic. See the modern-cpp skill's section 9.
#if defined(__has_warning)
#if __has_warning("-Wfunction-effects")
#define ESPRESSO_ASSUME_NONBLOCKING_BEGIN                                                                    \
    _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wfunction-effects\"")
/// Closes a ESPRESSO_ASSUME_NONBLOCKING_BEGIN region.
#define ESPRESSO_ASSUME_NONBLOCKING_END _Pragma("clang diagnostic pop")
#endif
#endif
#ifndef ESPRESSO_ASSUME_NONBLOCKING_BEGIN
#define ESPRESSO_ASSUME_NONBLOCKING_BEGIN
/// Closes a ESPRESSO_ASSUME_NONBLOCKING_BEGIN region.
#define ESPRESSO_ASSUME_NONBLOCKING_END
#endif

#if defined(__has_feature)
#if __has_feature(realtime_sanitizer)
#include <sanitizer/rtsan_interface.h>
/// Defined in a build with RealtimeSanitizer (-fsanitize=realtime).
#define ESPRESSO_REALTIME_SANITIZER
#endif
#endif

namespace primitives {

/// A type whose atomics the audio thread and the workers may use:
/// `std::atomic<T>` is lock-free on every processor the build targets
/// (`std::atomic<T>::is_always_lock_free`). On a target without the
/// instructions for the type's width, such as `std::atomic<double>` on a
/// 32-bit ARM core without `LDREXD`, every operation calls the library,
/// which takes a lock from a table of its own, against the real-time
/// rules (AGENTS.md). Each class that holds an atomic used on those
/// threads asserts its types beside it: `static_assert(realtimeAtomic<float>);`.
template <typename T>
concept realtimeAtomic = std::atomic<T>::is_always_lock_free;

/// The alignment, in bytes, of data that one thread writes and others
/// read or write on the audio thread's path: a queue's positions, the
/// worker pool's counters, each thread's busy time. 128 bytes keeps two
/// such members off one unit of coherence on the processors the engine
/// targets: on x86-64, Intel's L2 spatial prefetcher fetches 64-byte lines
/// in aligned pairs, so members 64 bytes apart still move between cores
/// together; Apple's M-series cores and some other ARM cores have 128-byte
/// lines. `std::hardware_destructive_interference_size` is not used: its
/// value may change with the compiler's tuning flags, which would change
/// the layout of classes in headers between translation units.
inline constexpr std::size_t cacheLine = 128;

/// Keeps RealtimeSanitizer from reporting the calling thread for its
/// scope (`__rtsan::ScopedDisabler`) in a build with -fsanitize=realtime,
/// and does nothing in any other build. It covers one call that departs
/// from the real-time rules, is listed in the modern-cpp skill's section
/// 9, and is a system call the sanitizer intercepts: the wake of a
/// sleeping worker, a futex call on Linux. Declare it just before that
/// call, inside the ESPRESSO_ASSUME_NONBLOCKING region that covers the
/// call for the static analysis, in a scope that ends after the call.
class [[maybe_unused]] realtimeSanitizerOff {
public:
    /// Turns the sanitizer's reports off until the destructor.
    realtimeSanitizerOff() = default;

    realtimeSanitizerOff(const realtimeSanitizerOff&) = delete;
    realtimeSanitizerOff& operator=(const realtimeSanitizerOff&) = delete;

private:
#ifdef ESPRESSO_REALTIME_SANITIZER
    __rtsan::ScopedDisabler disabler_;
#endif
};

}  // namespace primitives
