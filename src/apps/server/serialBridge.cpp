#include "serialBridge.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <espresso/primitives/io.h>
#include <espresso/protocol/jthread.h>

#include "fileHandle.h"
#include "ioStats.h"
#include "serialLines.h"

#include <poll.h>
#include <termios.h>

namespace server {

namespace fs = std::filesystem;

namespace {

// How long the bridge waits for a device between rounds; also the longest
// a queued output line waits.
constexpr int pollMs = 10;
// Reads from one device per round, so a flood cannot hold the thread.
constexpr int readsPerRound = 16;

// The termios speed for @p baud, one of io.md §4.2's rates.
std::optional<speed_t> speedOf(std::uint32_t baud) noexcept {
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
#if defined(B460800) && defined(B921600)
    case 460800: return B460800;
    case 921600: return B921600;
#elif defined(__APPLE__)
    // macOS takes any rate as its number.
    case 460800:
    case 921600: return static_cast<speed_t>(baud);
#endif
    default: return std::nullopt;
    }
}

std::string errnoText(int code) { return std::error_code(code, std::generic_category()).message(); }

// Opens @p path raw, 8 data bits, no parity, 1 stop bit, no flow control,
// at @p baud (io.md §4.2); nothing, with the reason in @p error, when it
// cannot.
std::optional<fileHandle> openSerial(const std::string& path, std::uint32_t baud, std::string& error) {
    // POSIX file and terminal calls take paths and settings by pointer
    // (modern-cpp skill §9); the descriptor is owned by fileHandle at once.
    fileHandle fd(::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
    if (fd.get() < 0) {
        const int code = errno;
        error = errnoText(code);
#if defined(__linux__)
        if (code == EACCES)
            error += "; add your user to the dialout group (uucp on some distributions) and log in again";
#endif
        return std::nullopt;
    }
    termios t{};
    if (::tcgetattr(fd.get(), &t) != 0) {
        error = "not a serial device";
        return std::nullopt;
    }
    const auto speed = speedOf(baud);
    if (!speed) {
        error = std::to_string(baud) + " baud is not supported here";
        return std::nullopt;
    }
    ::cfmakeraw(&t);
    t.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD | CS8);
    t.c_cflag &= ~static_cast<tcflag_t>(CSTOPB | PARENB | CRTSCTS);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (::cfsetispeed(&t, *speed) != 0 || ::cfsetospeed(&t, *speed) != 0 ||
        ::tcsetattr(fd.get(), TCSANOW, &t) != 0) {
        error = errnoText(errno);
        return std::nullopt;
    }
    return fd;
}

// The entries of @p dir whose names begin with one of @p prefixes, as
// paths, sorted.
std::vector<std::string> entries(const fs::path& dir, std::span<const std::string_view> prefixes) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (std::ranges::any_of(prefixes, [&](std::string_view p) { return name.starts_with(p); }))
            out.push_back(it->path().string());
    }
    std::ranges::sort(out);
    return out;
}

}  // namespace

std::vector<std::string> usbSerialDevices() {
#if defined(__APPLE__)
    constexpr auto prefixes = std::to_array<std::string_view>({"cu.usbmodem", "cu.usbserial"});
#else
    constexpr auto prefixes = std::to_array<std::string_view>({"ttyACM", "ttyUSB"});
#endif
    return entries("/dev", prefixes);
}

std::string resolveDevice(std::string_view pattern, const std::vector<std::string>& usbDevices) {
    if (pattern.empty()) return usbDevices.empty() ? std::string() : usbDevices.front();
    if (pattern.ends_with('*')) {
        const fs::path prefix(pattern.substr(0, pattern.size() - 1));
        const std::string name = prefix.filename().string();
        const auto dir = prefix.parent_path().empty() ? fs::path(".") : prefix.parent_path();
        const auto found = entries(dir, std::array<std::string_view, 1>{name});
        return found.empty() ? std::string() : found.front();
    }
    std::error_code ec;
    return fs::exists(fs::path(pattern), ec) ? std::string(pattern) : std::string();
}

// An open device: its path, its descriptor, its speed, the line it is
// reading, and the output it has not taken yet.
struct serialBridge::device {
    std::string path;
    std::vector<std::string> readers;  // nodes reading it, through any pattern
    std::vector<std::string> writers;  // nodes writing it
    fileHandle fd;
    std::uint32_t baud = 0;
    lineSplitter lines;
    std::string pending;
};

serialBridge::serialBridge(primitives::ioHub& io, ioStats& stats, serialOptions options)
    : io_(io),
      stats_(stats),
      options_(std::move(options)),
      thread_([this](const espresso::stop_token& stop) { run(stop); }) {}

serialBridge::~serialBridge() = default;

serialCounts serialBridge::counts() const noexcept {
    return {.lines = counts_.lines.load(),
            .malformed = counts_.malformed.load(),
            .tooLong = counts_.tooLong.load(),
            .disconnects = counts_.disconnects.load(),
            .sent = counts_.sent.load(),
            .dropped = counts_.dropped.load()};
}

void serialBridge::problem(const std::string& key, const std::vector<std::string>& nodes,
                           const std::string& message) {
    if (reported_.insert(key).second) stats_.error(nodes, message);
}

void serialBridge::close(const std::string& path, bool disconnected) {
    const auto it = devices_.find(path);
    if (it == devices_.end()) return;
    std::vector<std::string> nodes = it->second->readers;
    nodes.insert(nodes.end(), it->second->writers.begin(), it->second->writers.end());
    devices_.erase(it);
    if (!disconnected) return;
    counts_.disconnects.fetch_add(1);
    stats_.add(nodes, ioCount::disconnects);
    problem(path, nodes, "serial device " + path + " disconnected; waiting for it to return");
}

void serialBridge::run(const espresso::stop_token& stop) {
    auto nextScan = std::chrono::steady_clock::now();
    std::vector<pollfd> polled;
    std::vector<std::string> paths;
    while (!stop.stop_requested()) {
        if (const auto now = std::chrono::steady_clock::now(); now >= nextScan) {
            rescan();
            nextScan = now + options_.rescan;
        }
        queueOutput();
        polled.clear();
        paths.clear();
        for (const auto& [path, d] : devices_) {
            const short events = d->pending.empty() ? POLLIN : static_cast<short>(POLLIN | POLLOUT);
            polled.push_back({.fd = d->fd.get(), .events = events, .revents = 0});
            paths.push_back(path);
        }
        // poll() takes its descriptors as an array (modern-cpp skill §9).
        if (::poll(polled.data(), static_cast<nfds_t>(polled.size()), pollMs) <= 0) continue;
        for (std::size_t i = 0; i < polled.size(); ++i) {
            const auto it = devices_.find(paths[i]);
            if (it == devices_.end()) continue;
            const auto revents = static_cast<unsigned>(polled[i].revents);
            if ((revents & static_cast<unsigned>(POLLIN)) != 0U) readFrom(*it->second);
            if ((revents & static_cast<unsigned>(POLLOUT)) != 0U) writeTo(*it->second);
            if ((revents & static_cast<unsigned>(POLLHUP | POLLERR | POLLNVAL)) != 0U) close(paths[i], true);
        }
    }
}

void serialBridge::rescan() {
    const auto usb = options_.usbDevices ? options_.usbDevices() : std::vector<std::string>();
    struct want {
        std::uint32_t baud = 0;
        std::vector<std::string> readers;
        std::vector<std::string> writers;
    };
    std::map<std::string, want> wanted;  // by path
    resolved_.clear();
    for (const auto& d : io_.serialDevices()) {
        std::vector<std::string> nodes = d.readers;
        nodes.insert(nodes.end(), d.writers.begin(), d.writers.end());
        const std::string key = "pattern " + d.pattern;
        const std::string path = resolveDevice(d.pattern, usb);
        if (path.empty()) {
            problem(key, nodes,
                    "no serial device matches " +
                        (d.pattern.empty() ? std::string("the default, the first USB serial device")
                                           : d.pattern) +
                        "; waiting for one");
            continue;
        }
        reported_.erase(key);
        resolved_.emplace(d.pattern, path);
        auto& w = wanted.try_emplace(path, want{.baud = d.baud, .readers = {}, .writers = {}}).first->second;
        w.readers.insert(w.readers.end(), d.readers.begin(), d.readers.end());
        w.writers.insert(w.writers.end(), d.writers.begin(), d.writers.end());
    }
    for (auto it = devices_.begin(); it != devices_.end();) {
        it = wanted.contains(it->first) ? std::next(it) : devices_.erase(it);
    }
    for (const auto& [path, w] : wanted) {
        if (const auto open = devices_.find(path); open != devices_.end()) {
            open->second->readers = w.readers;
            open->second->writers = w.writers;
            continue;
        }
        std::vector<std::string> nodes = w.readers;
        nodes.insert(nodes.end(), w.writers.begin(), w.writers.end());
        std::string error;
        auto fd = openSerial(path, w.baud, error);
        if (!fd) {
            std::string message = "cannot open serial device ";
            message += path;
            message += ": ";
            message += error;
            problem(path, nodes, message);
            continue;
        }
        reported_.erase(path);
        devices_.emplace(path, std::make_unique<device>(device{.path = path,
                                                               .readers = w.readers,
                                                               .writers = w.writers,
                                                               .fd = std::move(*fd),
                                                               .baud = w.baud,
                                                               .lines = {},
                                                               .pending = {}}));
    }
}

void serialBridge::readFrom(device& d) {
    std::array<char, 4096> buffer{};
    const std::uint64_t longBefore = d.lines.tooLong();
    for (int round = 0; round < readsPerRound; ++round) {
        // read() fills a buffer by pointer (modern-cpp skill §9).
        const auto n = ::read(d.fd.get(), buffer.data(), buffer.size());
        if (n <= 0) break;  // nothing more for now; a hang-up shows in poll()
        d.lines.feed(std::span(buffer).first(static_cast<std::size_t>(n)), [&](std::string_view line) {
            auto parsed = parseLine(line);
            if (parsed.kind == lineKind::malformed) {
                counts_.malformed.fetch_add(1);
                stats_.add(d.readers, ioCount::malformed);
            }
            if (parsed.kind != lineKind::frame) return;
            parsed.frame.time = options_.clock ? options_.clock() : 0.0;
            counts_.lines.fetch_add(1);
            stats_.add(d.readers, ioCount::frames);
            // Every port on the device hears the line (§4.5).
            for (const auto& [pattern, path] : resolved_) {
                if (path == d.path) io_.deliverSerial(pattern, parsed.frame);
            }
        });
    }
    const std::uint64_t tooLong = d.lines.tooLong() - longBefore;
    counts_.tooLong.fetch_add(tooLong);
    if (tooLong > 0) stats_.add(d.readers, ioCount::malformed, tooLong);
}

void serialBridge::queueOutput() {
    io_.drainSerialOut([&](const primitives::serialPort& port, const primitives::dataFrame& frame) {
        const auto path = resolved_.find(port.device());
        const auto d = path == resolved_.end() ? devices_.end() : devices_.find(path->second);
        const std::string line = formatLine(frame);
        // Lines join a device's output whole, so ports never interleave
        // mid-line (§4.5).
        if (d == devices_.end() || d->second->pending.size() + line.size() > maxPendingBytes) {
            counts_.dropped.fetch_add(1);
            stats_.add(port.owner(), ioCount::drops);
            io_.countDrop();
            return;
        }
        d->second->pending += line;
        counts_.sent.fetch_add(1);
        stats_.add(port.owner(), ioCount::frames);
    });
}

void serialBridge::writeTo(device& d) {
    // write() takes the bytes by pointer (modern-cpp skill §9).
    const auto n = ::write(d.fd.get(), d.pending.data(), d.pending.size());
    if (n > 0) d.pending.erase(0, static_cast<std::size_t>(n));
}

}  // namespace server
