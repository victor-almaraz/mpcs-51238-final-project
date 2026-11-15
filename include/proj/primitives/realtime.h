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
