// Timeouts and waits for tests that depend on another thread or process.

#pragma once

#include <chrono>
#include <thread>

namespace support {

/// True in an AddressSanitizer or ThreadSanitizer build, which runs several
/// times slower (ThreadSanitizer 5 to 15 times), so its waits are longer.
constexpr bool sanitized() noexcept {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

/// How much longer waits are in a sanitizer build.
inline constexpr int slowdown = sanitized() ? 5 : 1;

/// How long a client waits for each reply from a test server on another
/// thread. Long enough for a machine that runs the whole suite in parallel
/// on fewer cores than jobs; a reply ends the wait at once, so a passing
/// run takes no longer. A test that drops a message to exercise a retry
/// waits this long once.
inline constexpr int replyTimeoutMs = 1000 * slowdown;

/// How long a test waits for another thread or process to reach a state:
/// long enough for a loaded machine or a sanitizer build. A wait ends as
/// soon as the state is reached, so a passing run takes no longer.
inline constexpr std::chrono::milliseconds settleTimeout{10'000 * slowdown};

/// Checks @p reached every 2 ms until it holds or @p timeout passes;
/// returns whether it held.
template <typename Condition>
bool waitUntil(Condition reached, std::chrono::milliseconds timeout = settleTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!reached()) {
        if (std::chrono::steady_clock::now() >= deadline) return reached();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

}  // namespace support
