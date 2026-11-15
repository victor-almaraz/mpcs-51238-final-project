---
name: modern-cpp
description: Modern C++20 practices for this repository — RAII for every resource, smart pointers for all ownership and indirection, self-joining threads (proj::jthread), scoped locks, value semantics, and the real-time constraints of the audio thread. Use whenever writing, reviewing, or refactoring C++ in this repository, including tests, before committing C++ changes, and when a design seems to need a raw pointer, new/delete, a manual join, or a manual lock.
---

# Modern C++ in this repository

These rules apply to all C++ in `include/`, `src/`, and `tests/`. They are defaults with one escape hatch: a **narrow unsafe use**, allowed only where the rule cannot be followed (an OS or driver API) or where measurement shows a performance problem. Every narrow unsafe use is wrapped in the smallest possible type or function, commented with the reason, and listed in the last section of this file.

Read `AGENTS.md` (real-time rules) and `local-docs/lessons.md` first. Where these rules meet the audio thread, the audio-thread section below decides.

## 1. Every resource has an owner object (RAII)

Memory, threads, locks, file descriptors, sockets, devices, signal masks, floating-point modes, and registrations each belong to an object whose destructor releases them.

- Acquire in the constructor, release in the destructor. Never pair `open`/`close`, `lock`/`unlock`, `start`/`stop`, or `register`/`unregister` by hand across a function.
- Prefer the Rule of Zero: compose members that already manage themselves (`std::unique_ptr`, `std::vector`, `proj::jthread`, `std::optional`) so no destructor is needed.
- A class that does manage a resource directly follows the Rule of Five: delete copying unless a copy is meaningful, and make moves `noexcept`.
- Member order is part of correctness. Members are destroyed in reverse order: declare a thread or a callback source **after** everything it uses, so it stops first.
- Destructors do not throw and do not block indefinitely.

## 2. Ownership: smart pointers only

- No `new` or `delete` in project code. Use `std::make_unique` and `std::make_shared`. A type with a private constructor uses a passkey argument, not `new`.
- `std::unique_ptr<T>` is the default owner. Transfer ownership by moving it.
- `std::shared_ptr<T>` is for real shared ownership: several owners, none of which outlives the others by design. Cycles of `shared_ptr` leak; break them with `std::weak_ptr` or by clearing the members of every object before the owning container is destroyed.
- `std::weak_ptr<T>` is for registries and caches that must not keep objects alive. Lock it, use the `shared_ptr`, and drop it.
- `.release()` and `.get()` are narrow unsafe uses (see section 9).

## 3. Indirection without ownership

No raw pointers (`T*`) for non-owning access either.

| Need | Use |
|---|---|
| Always present, never reseated | `T&` member or parameter |
| Always present, reseatable or stored in a container | `std::reference_wrapper<T>` |
| May be absent (lookup results, optional ports) | `metadata::optionalRef<T>` (`std::optional<std::reference_wrapper<const T>>`), or `std::optional<T>` for small values |
| A sequence | `std::span<T>` / `std::span<const T>` |
| Text | `std::string_view` (never stored beyond the life of its source) |
| Shared lifetime across threads or owners | `std::shared_ptr<T>` / `std::weak_ptr<T>` |
| An element of an owned container | an index or a stable ID, looked up when used |

- Out-parameters are references (`std::string& error`) or, better, part of the return value (`std::optional<T>`, a result struct).
- Do not use `std::get_if` (it returns a pointer). Use `std::holds_alternative` with `std::get`, or `std::visit`.
- Callbacks are `std::function` or templates, not function pointers. Pointer-to-member tables become `std::function` or lambdas.
- Casting through `reinterpret_cast` is a narrow unsafe use. Convert element by element instead (for example, build a `std::string` from a `std::uint8_t` range).

## 4. Threads: proj::jthread

- Every thread is a `proj::jthread` (`<proj/protocol/jthread.h>`): the subset of `std::jthread`'s interface the project uses, on a POSIX thread with a stack of `threadStackBytes` (8 MiB) on every platform. `std::jthread` cannot set a stack size, and secondary-thread stacks otherwise differ (512 KiB on macOS). Never use `std::thread` directly and never `detach()`.
- A thread function takes `const proj::stop_token&` and loops on `!stop.stop_requested()`. Do not add a separate `std::atomic<bool> stop_` flag.
- A blocking wait must be woken on stop: a timed wait (`wait_for`), a notify after `request_stop()` (under the same mutex the waiter uses), or a signal to the thread (`pthread_kill`) for `sigwait`.
- The `jthread` member is the **last** data member of its owner (section 1).

## 5. Locks: scoped only

- `std::lock_guard` (or `std::scoped_lock` for several mutexes) for a scope. `std::unique_lock` only where a condition variable needs it.
- Never call `.lock()` or `.unlock()` on a mutex or on a lock object. To do work outside the lock, end the scope: copy or move what is needed out of the protected state inside a block, then work after it.
- Condition-variable waits always use the predicate overload.
- Prefer `const std::lock_guard lock(mutex_);` and keep critical sections short; never call user callbacks while holding a lock.

## 6. Values, types, and interfaces

- Prefer values and `std::optional`, `std::variant`, `std::array`, `std::vector` to heap objects. Return by value; rely on moves and copy elision.
- `const` everything that does not change; mark functions `noexcept` when they cannot throw, and `[[nodiscard]]` when ignoring the result is a bug.
- Use `enum class`, `std::span`, `std::string_view`, designated initializers, and `constexpr` tables (data over code).
- No C-style casts; `static_cast` for conversions, explicit and narrow. Index containers with `std::size_t`.
- Initialize every member at its declaration.
- Keep headers self-contained and include what you use.

## 7. Errors

- Errors cross module boundaries as values (`std::optional`, error structs, result structs). Exceptions stay inside a module's control-thread code (the builder, the compiler) and are caught at its boundary.
- Never ignore a failure silently: report it, count it, or return it.

## 8. The audio thread

Smart types are compatible with the real-time rules when used this way:

- **No frees on the audio thread.** Ownership moves through lock-free queues as `std::unique_ptr` (`spscQueue<std::unique_ptr<T>>`, moved in and out); the control thread destroys. A `unique_ptr` on the audio thread is only moved from or into an empty one. If a hand-off queue is full, keep the object and retry; do not destroy it.
- `std::shared_ptr` held by audio-thread objects is never copied or destroyed on the audio thread; access it by `const&` or `*`. Its last owner is always on a control thread.
- No locks, no `std::function` construction, no container growth, no `std::string` building, no I/O.
- `proj::jthread` and RAII guards (such as `denormalGuard`) are fine: they act at scope edges, not per sample.
- Fetch spans once per block; do not rebuild them per sample.

## 9. Narrow unsafe uses

Allowed only where listed here. Each site has a comment saying why.

| Where | What | Why |
|---|---|---|
| `src/protocol/udp.cpp` (`asSockaddr`) | `reinterpret_cast<sockaddr*>` (`NOLINTNEXTLINE`) | The POSIX socket API takes addresses by pointer; the two helpers are the only casts |
| `src/apps/signals.h` (`signalMask`, `signalWaiter`) | `&set_`, `pthread_kill(SIGTERM)` (`NOLINTNEXTLINE(bugprone-bad-signal-to-kill-thread)`) | The POSIX signal API takes sets by pointer; SIGTERM is blocked in every thread, so it wakes `sigwait` instead of terminating |
| `include/proj/protocol/jthread.h` (`jthread::run`) | `void*` start routine argument, `pthread_attr_t` by address | POSIX threads are the only portable way to set a thread's stack size (`threadStackBytes`); the argument is the owned `body_` |
| `src/apps/server/devices.cpp`, `src/apps/server/deviceBlock.h` | `float*` channel arrays, converted to spans at once | The audio driver (JUCE) passes device buffers as raw pointers |
| `src/apps/server/devices.cpp`, `src/apps/server/main.cpp` | JUCE `MidiInput*`, `AudioIODevice*`, `getRawData()`, `openDevice(id, this)`, `addAudioCallback(&callback)`, `initialise(…, nullptr, …, &setup)` | JUCE returns and takes these as raw pointers; none is stored or owned, and the callback registration is an RAII guard |
| `src/apps/arguments.h` | `char** argv` | `main` receives it; `apps::arguments` turns it into string views first |
| `src/tui/settings.cpp` (`environmentVariable`) | `const char*` from `std::getenv` | The C library returns environment values by pointer |
| `tests/interpreter/examples_test.cpp` | `std::getenv` presence check (`NOLINTNEXTLINE(concurrency-mt-unsafe)`) | The C library returns environment values by pointer; only presence is read, and nothing in the suite sets the environment |
| `src/tui/fileStore.cpp` (`writeAtomic`, `temporaryFile`) | File descriptors, `stat` by address, `write` from a pointer, `umask` | POSIX file calls take C strings, buffers, and results by pointer; the descriptor is owned by `temporaryFile` |
| `src/interpreter/reader.cpp` | `std::strtod(…, nullptr)` | libc++ on the build host lacks floating-point `std::from_chars` |
| `include/proj/primitives/realtime.h` | `PROJ_NONBLOCKING` macro | An attribute cannot be aliased, and compilers without `clang::nonblocking` reject it under `-Werror` |
| `tests/server/apps_test.cpp`, `tests/server/offline_test.cpp` | A `std::vector<float*>` emulating the driver's channel array; `getrlimit`/`setrlimit` by address and `std::signal(SIGXFSZ, …)` in the `fileSizeLimit` guard | Tests reproduce the driver and OS interfaces they exercise |

Silence a clang-tidy finding only at a site in this table, with `// NOLINTNEXTLINE(check): reason` on the line above (a trailing comment can be wrapped away from its line by clang-format).

Adding a row requires a reason in the code and, for performance, a measurement.

## Checklist before committing C++

- [ ] `grep -nE "\bnew |delete |std::thread\b|\.detach\(|\.lock\(\)|\.unlock\(\)|get_if" ` finds nothing outside section 9.
- [ ] No raw `T*` members, parameters, or return types outside section 9.
- [ ] Each thread is a `proj::jthread`, declared last; each wait wakes on stop.
- [ ] Nothing on the audio thread allocates, frees, locks, or destroys a smart pointer's last owner.
- [ ] `-DPROJ_WARNINGS_AS_ERRORS=ON` builds, all tests pass, and Doxygen reports no warnings.
- [ ] `git clang-format --diff` reports nothing for the changed lines.
- [ ] clang-tidy reports nothing for the changed files, except `NOLINT` sites listed in section 9.
- [ ] Every suite passes under AddressSanitizer and UndefinedBehaviorSanitizer.
- [ ] After a change to threads, queues, or ownership: every suite passes under ThreadSanitizer.
- [ ] After a change to audio-thread code: it is marked `[[clang::nonblocking]]` and passes under RealtimeSanitizer.

Use the LLVM tools wherever they are installed (AGENTS.md "Clang tooling"). Where one is not installed, say which check was skipped.
