// The serial bridge (io.md §4): opens the devices that serial ports ask
// for, delivers the numbers of each line they send, and writes the lines
// serialOut nodes queue. POSIX termios and poll(), no JUCE.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/primitives/io.h>
#include <espresso/protocol/jthread.h>

#include "ioStats.h"

namespace server {

/// The USB serial devices under /dev, sorted: `ttyACM*` and `ttyUSB*` on
/// Linux, `cu.usbmodem*` and `cu.usbserial*` on macOS.
std::vector<std::string> usbSerialDevices();

/// The device @p pattern names (io.md §4.1): the path itself when it
/// exists; for a path ending in `*`, the first entry, in sorted order, of
/// its directory whose path begins with the rest; for the empty pattern,
/// the first of @p usbDevices. Empty when none does.
std::string resolveDevice(std::string_view pattern, const std::vector<std::string>& usbDevices);

/// Settings for a serial bridge.
struct serialOptions {
    /// The device clock, in seconds, that stamps each frame received.
    std::function<double()> clock;
    /// Lists the devices the empty pattern chooses from.
    std::function<std::vector<std::string>()> usbDevices = usbSerialDevices;
    /// How often patterns are resolved again and missing devices reopened
    /// (io.md §4.6).
    std::chrono::milliseconds rescan{1000};
};

/// Moves lines between serial devices and the hub's serial ports, on its
/// own thread. One file descriptor serves every port on a device (§4.5).
/// A device that goes away is closed and counted; it opens again when it
/// returns, and the ports waiting for it stay open (§4.6).
class serialBridge {
public:
    /// Bytes of output held for a device that reads slowly; a line that
    /// does not fit is dropped.
    static constexpr std::size_t maxPendingBytes = std::size_t{64} << 10U;

    /// Starts the bridge between @p io and its devices, counting what each
    /// node receives, sends, and loses in @p stats (io.md §10). A device
    /// that is missing or cannot open counts one error for its nodes until
    /// it opens.
    serialBridge(primitives::ioHub& io, ioStats& stats, serialOptions options);
    ~serialBridge();

    serialBridge(const serialBridge&) = delete;
    serialBridge& operator=(const serialBridge&) = delete;
    serialBridge(serialBridge&&) = delete;
    serialBridge& operator=(serialBridge&&) = delete;

private:
    struct device;

    void run(const espresso::stop_token& stop);
    void rescan();
    void readFrom(device& d);
    void queueOutput();
    void writeTo(device& d);
    void close(const std::string& path, bool disconnected);
    // Counts an error for @p nodes once, until @p key leaves @p reported.
    void problem(std::set<std::string>& reported, const std::string& key,
                 const std::vector<std::string>& nodes, const std::string& message);

    primitives::ioHub& io_;
    ioStats& stats_;
    serialOptions options_;
    // Bridge thread only.
    std::map<std::string, std::unique_ptr<device>> devices_;  // by path
    std::map<std::string, std::string> resolved_;             // pattern to path
    // Problems counted once until they clear (io.md §10.2).
    std::set<std::string> unmatched_;  // patterns that match no device
    std::set<std::string> failed_;     // devices that cannot open or went away
    espresso::jthread thread_;  // last: stops and joins before the rest is destroyed
};

}  // namespace server
