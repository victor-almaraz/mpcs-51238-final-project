// The machine's processor cores, for the default thread count
// (threads.md §7.1). Has no JUCE dependency.

#pragma once

namespace server {

/// The machine's physical processor cores, at least 1: from `sysctl` on
/// macOS and `/sys/devices/system/cpu` on Linux, or the hardware threads
/// when neither answers.
int physicalCores();

/// The default of `--threads` (threads.md §7.1, proposed): the physical
/// cores, at most 16.
int defaultThreads();

}  // namespace server
