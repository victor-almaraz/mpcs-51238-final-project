// Timeouts for tests that wait on another thread.

#pragma once

namespace support {

/// How long a client waits for each reply from a test server on another
/// thread. Long enough for a machine that runs the whole suite in parallel
/// on fewer cores than jobs; a reply ends the wait at once, so a passing
/// run takes no longer. A test that drops a message to exercise a retry
/// waits this long once.
inline constexpr int replyTimeoutMs = 1000;

}  // namespace support
