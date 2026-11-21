/// @file
/// The marker for functions that run on the audio thread (AGENTS.md,
/// real-time rules). Clang's function-effect analysis (-Wfunction-effects)
/// checks a marked function statically, and RealtimeSanitizer
/// (-fsanitize=realtime) aborts when one allocates, frees, locks, or makes
/// a system call at run time; realtimeSanitizerOff exempts one listed
/// call from the sanitizer.

#pragma once

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
