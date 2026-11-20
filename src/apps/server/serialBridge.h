// The serial bridge (io.md §4): opens the devices that serial ports ask
// for, delivers the numbers of each line they send, and writes the lines
// serialOut nodes queue. POSIX termios and poll(), no JUCE.

#pragma once

#include <atomic>
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

namespace server {

/// The USB serial devices under /dev, sorted: `ttyACM*` and `ttyUSB*` on
/// Linux, `cu.usbmodem*` and `cu.usbserial*` on macOS.
std::vector<std::string> usbSerialDevices();

/// The device @p pattern names (io.md §4.1): the path itself when it
/// exists; for a path ending in `*`, the first entry, in sorted order, of
/// its directory whose path begins with the rest; for the empty pattern,
/// the first of @p usbDevices. Empty when none does.
std::string resolveDevice(std::string_view pattern, const std::vector<std::string>& usbDevices);

/// What the bridge has counted, over every device (io.md §10.1).
struct serialCounts {
    std::uint64_t lines = 0;        ///< Lines delivered as frames
    std::uint64_t malformed = 0;    ///< Lines with a field that is not a number, or too many fields
    std::uint64_t tooLong = 0;      ///< Lines longer than maxLineBytes
    std::uint64_t disconnects = 0;  ///< Devices that went away while open
    std::uint64_t sent = 0;         ///< Lines queued for a device
    std::uint64_t dropped = 0;      ///< Output lines with no device to go to, or that did not fit
};

/// Settings for a serial bridge.
struct serialOptions {
    /// The device clock, in seconds, that stamps each frame received.
    std::function<double()> clock;
    /// Receives each error once, until the device it concerns opens.
    std::function<void(std::string_view)> report;
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

    /// Starts the bridge between @p io and its devices.
    serialBridge(primitives::ioHub& io, serialOptions options);
    ~serialBridge();

    serialBridge(const serialBridge&) = delete;
    serialBridge& operator=(const serialBridge&) = delete;
    serialBridge(serialBridge&&) = delete;
    serialBridge& operator=(serialBridge&&) = delete;

    /// What the bridge has counted so far.
    serialCounts counts() const noexcept;

private:
    struct device;

    void run(const espresso::stop_token& stop);
    void rescan();
    void readFrom(device& d);
    void queueOutput();
    void writeTo(device& d);
    void close(const std::string& path, bool disconnected);
    void report(const std::string& key, const std::string& message);

    primitives::ioHub& io_;
    serialOptions options_;
    // Bridge thread only.
    std::map<std::string, std::unique_ptr<device>> devices_;  // by path
    std::map<std::string, std::string> resolved_;             // pattern to path
    std::set<std::string> reported_;                          // errors reported, by device or pattern
    // Written by the bridge thread, read by any.
    struct atomicCounts {
        std::atomic<std::uint64_t> lines{0};
        std::atomic<std::uint64_t> malformed{0};
        std::atomic<std::uint64_t> tooLong{0};
        std::atomic<std::uint64_t> disconnects{0};
        std::atomic<std::uint64_t> sent{0};
        std::atomic<std::uint64_t> dropped{0};
    } counts_;
    espresso::jthread thread_;  // last: stops and joins before the rest is destroyed
};

}  // namespace server
