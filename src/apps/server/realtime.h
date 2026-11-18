// Real-time scheduling for a thread the server starts itself: the
// no-audio timer, which stands in for a device's audio thread. Has no
// JUCE dependency.

#pragma once

#include <chrono>
#include <string>

namespace server {

/// Asks the operating system to schedule the calling thread as a
/// real-time thread that needs up to @p computation of every @p period:
/// a Mach time-constraint policy on macOS, SCHED_FIFO on Linux. Returns
/// an empty string when it is granted, or why it was not.
std::string makeCurrentThreadRealtime(std::chrono::nanoseconds period, std::chrono::nanoseconds computation);

}  // namespace server
