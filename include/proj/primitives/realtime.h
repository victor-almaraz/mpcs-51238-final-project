/// @file
/// The marker for functions that run on the audio thread (AGENTS.md,
/// real-time rules). Clang's function-effect analysis (-Wfunction-effects)
/// checks a marked function statically, and RealtimeSanitizer
/// (-fsanitize=realtime) aborts when one allocates, frees, locks, or makes
/// a system call at run time.

#pragma once

/// Marks a function as nonblocking: `void f() noexcept PROJ_NONBLOCKING;`,
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
#define PROJ_NONBLOCKING [[clang::nonblocking]]
#endif
#endif
#ifndef PROJ_NONBLOCKING
#define PROJ_NONBLOCKING
#endif

/// Brackets code in a nonblocking function that Clang's analysis cannot
/// show to be nonblocking but an invariant does: the destructor of a smart
/// pointer that is empty by then, a queue slot the other end emptied, a
/// library call whose storage was reserved beforehand. Each use states
/// its invariant in a comment; RealtimeSanitizer still checks the code at
/// run time. Expands to a diagnostic push that ignores -Wfunction-effects
/// where the compiler has that warning, and to nothing elsewhere.
///
/// A macro, like PROJ_NONBLOCKING, because only `_Pragma` can scope a
/// diagnostic. See the modern-cpp skill's section 9.
#if defined(__has_warning)
#if __has_warning("-Wfunction-effects")
#define PROJ_ASSUME_NONBLOCKING_BEGIN                                                                        \
    _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wfunction-effects\"")
/// Closes a PROJ_ASSUME_NONBLOCKING_BEGIN region.
#define PROJ_ASSUME_NONBLOCKING_END _Pragma("clang diagnostic pop")
#endif
#endif
#ifndef PROJ_ASSUME_NONBLOCKING_BEGIN
#define PROJ_ASSUME_NONBLOCKING_BEGIN
/// Closes a PROJ_ASSUME_NONBLOCKING_BEGIN region.
#define PROJ_ASSUME_NONBLOCKING_END
#endif
