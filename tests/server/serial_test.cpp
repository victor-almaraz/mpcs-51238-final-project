// Serial lines and the serial bridge, against pseudo-terminals (io.md §4,
// §14.1).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <espresso/primitives/io.h>

#include "ioStats.h"
#include "serialBridge.h"
#include "serialLines.h"
#include "support/tempDir.h"
#include "support/timing.h"
#include "support/unwrap.h"

#include <poll.h>
// NOLINTNEXTLINE(modernize-deprecated-headers): POSIX declares posix_openpt here
#include <stdlib.h>
#include <termios.h>

using namespace server;
using primitives::dataFrame;

namespace {

std::vector<float> valuesOf(const dataFrame& f) {
    const auto used = std::span(f.values).first(f.count);
    return {used.begin(), used.end()};
}

dataFrame frameOf(std::initializer_list<float> values) {
    dataFrame f;
    for (const float v : values) f.values[f.count++] = v;
    return f;
}

std::vector<std::string> split(lineSplitter& s, std::string_view bytes) {
    std::vector<std::string> out;
    s.feed(bytes, [&](std::string_view line) { out.emplace_back(line); });
    return out;
}

}  // namespace

TEST_CASE("lines end at line feeds without a carriage return before them (io.md 4.3)") {
    lineSplitter s;
    CHECK(split(s, "512 0.25 1\r\n7").empty() == false);
    CHECK(split(s, "\n\n") == std::vector<std::string>{"7", ""});
    CHECK(split(s, "a\rb\n") == std::vector<std::string>{"a\rb"});  // only a carriage return before the feed
    CHECK(s.takeTooLong() == 0);
}

TEST_CASE("a line longer than 256 bytes is dropped up to the next line feed and counted") {
    lineSplitter s;
    const std::string limit(maxLineBytes, '1');
    CHECK(split(s, limit + "\r\n") == std::vector<std::string>{limit});
    CHECK(split(s, limit + "1\n").empty());
    CHECK(s.takeTooLong() == 1);
    CHECK(s.takeTooLong() == 0);  // taken
    // Long in pieces, then a line after it.
    CHECK(split(s, std::string(200, '2')).empty());
    CHECK(split(s, std::string(200, '2')).empty());
    CHECK(split(s, "\n3\n") == std::vector<std::string>{"3"});
    CHECK(s.takeTooLong() == 1);
}

TEST_CASE("a line's numbers are separated by spaces tabs or commas") {
    for (const std::string_view line :
         {"512 0.25 1", "512,0.25,1", "512, 0.25,\t1", "  512\t0.25 1  ", "512,,0.25 ,1"}) {
        INFO(line);
        const auto p = parseLine(line);
        REQUIRE(p.kind == lineKind::frame);
        CHECK(valuesOf(p.frame) == std::vector<float>{512.0f, 0.25f, 1.0f});
    }
    CHECK(valuesOf(parseLine("-3.5e2 +7 .5 5. 1E-3").frame) ==
          std::vector<float>{-350.0f, 7.0f, 0.5f, 5.0f, 1e-3f});
    CHECK(parseLine("").kind == lineKind::empty);
    CHECK(parseLine(" ,\t").kind == lineKind::empty);
    CHECK(parseLine("1 2 3 4 5 6 7 8").kind == lineKind::frame);
}

TEST_CASE("a line with a field that is not a finite number or with more than 8 fields is malformed") {
    for (const std::string_view line : {"1 x", "inf", "nan", "-INF", "0x10", "1e", "1e+", ".", "-", "1.2.3",
                                        "3e39", "1 2 3 4 5 6 7 8 9", "12a"}) {
        INFO(line);
        CHECK(parseLine(line).kind == lineKind::malformed);
    }
}

TEST_CASE("frames are written as lines of the shortest numbers that read back the same (io.md 4.4)") {
    CHECK(formatLine(frameOf({1.0f, 0.5f, -2.0f})) == "1 0.5 -2\n");
    CHECK(formatLine(frameOf({0.1f})) == "0.1\n");
    CHECK(formatLine(frameOf({})) == "\n");
    for (const float v :
         {0.1f, 1.0f / 3.0f, 1e-7f, -2.5f, 16777216.0f, 3.4e38f, std::numeric_limits<float>::min(),
          std::numeric_limits<float>::denorm_min(), 123456.79f}) {
        INFO(v);
        const auto line = formatLine(frameOf({v}));
        const auto back = parseLine(std::string_view(line).substr(0, line.size() - 1));
        REQUIRE(back.kind == lineKind::frame);
        CHECK(back.frame.values[0] == v);
    }
}

TEST_CASE("a device pattern names a path a first match of a prefix or the first USB device (io.md 4.1)") {
    const support::tempDir dir;
    for (const auto* name : {"board2", "board1", "other"}) std::ofstream(dir / name) << "";
    const std::string root = dir.path().string();
    CHECK(resolveDevice(root + "/board*", {}) == root + "/board1");
    CHECK(resolveDevice(root + "/oth*", {}) == root + "/other");
    CHECK(resolveDevice(root + "/none*", {}).empty());
    CHECK(resolveDevice(root + "/board2", {}) == root + "/board2");
    CHECK(resolveDevice(root + "/board3", {}).empty());
    CHECK(resolveDevice("", {"/dev/ttyACM0", "/dev/ttyACM1"}) == "/dev/ttyACM0");
    CHECK(resolveDevice("", {}).empty());
}

namespace {

// A pseudo-terminal: the test holds the master side; the bridge opens the
// slave by path. The slave is raw, as the bridge sets it, so what the test
// writes before the bridge opens it is still there.
class pty {
public:
    pty() : master_(::posix_openpt(O_RDWR | O_NOCTTY)) {
        REQUIRE(master_ >= 0);
        REQUIRE(::grantpt(master_) == 0);
        REQUIRE(::unlockpt(master_) == 0);
        slavePath_ = ::ptsname(master_);  // NOLINT(concurrency-mt-unsafe): tests open terminals on one thread
        slave_ = ::open(slavePath_.c_str(), O_RDWR | O_NOCTTY);
        REQUIRE(slave_ >= 0);
        termios t{};
        REQUIRE(::tcgetattr(slave_, &t) == 0);
        ::cfmakeraw(&t);
        REQUIRE(::tcsetattr(slave_, TCSANOW, &t) == 0);
        REQUIRE(::fcntl(master_, F_SETFL, O_NONBLOCK) == 0);
    }
    ~pty() { close(); }
    pty(const pty&) = delete;
    pty& operator=(const pty&) = delete;
    pty(pty&&) = delete;
    pty& operator=(pty&&) = delete;

    const std::string& path() const noexcept { return slavePath_; }

    void write(std::string_view bytes) const {
        REQUIRE(::write(master_, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
    }

    // What the bridge has written so far.
    std::string read() const {
        std::string out;
        std::array<char, 256> buffer{};
        while (true) {
            const auto n = ::read(master_, buffer.data(), buffer.size());
            if (n <= 0) break;
            out.append(buffer.data(), static_cast<std::size_t>(n));
        }
        return out;
    }

    // Hangs up, as an unplugged device does.
    void close() {
        if (slave_ >= 0) ::close(std::exchange(slave_, -1));
        if (master_ >= 0) ::close(std::exchange(master_, -1));
    }

private:
    int master_ = -1;
    int slave_ = -1;
    std::string slavePath_;
};

serialOptions fast() {
    return {.clock = [] { return 2.5; }, .usbDevices = {}, .rescan = std::chrono::milliseconds(20)};
}

// What @p stats reports, as `node: message (count)` lines: everything due
// an hour from now.
std::string reported(ioStats& stats) {
    std::string out;
    for (const auto& e : stats.due(std::chrono::steady_clock::now() + std::chrono::hours(1)))
        out += e.node + ": " + e.message + " (" + std::to_string(e.count) + ")\n";
    return out;
}

std::optional<dataFrame> next(primitives::serialPort& port) {
    dataFrame f;
    if (!support::waitUntil([&] { return port.queue.pop(f); })) return std::nullopt;
    return f;
}

}  // namespace

TEST_CASE("the serial bridge delivers a device's lines to every port on it (io.md 4.5)") {
    const support::tempDir dir;
    pty board;
    const std::string link = (dir / "board").string();
    std::filesystem::create_symlink(board.path(), link);
    primitives::ioHub hub;
    const auto exact = hub.openSerialIn("knob", link, 115200);
    const auto prefixed = hub.openSerialIn("fader", (dir / "boa*").string(), 115200);
    exact->setActive(true);
    prefixed->setActive(true);
    board.write("512 0.25 1\r\nnot a number\n\n");
    ioStats stats;
    const serialBridge bridge(hub, stats, fast());
    const dataFrame a = support::unwrap(next(*exact));
    CHECK(valuesOf(a) == std::vector<float>{512.0f, 0.25f, 1.0f});
    CHECK(a.time == 2.5);
    CHECK(valuesOf(support::unwrap(next(*prefixed))) == std::vector<float>{512.0f, 0.25f, 1.0f});
    CHECK(support::waitUntil([&] { return stats.of("knob")[ioCount::malformed] == 1; }));
    for (const auto* node : {"knob", "fader"}) {
        CHECK(stats.of(node)[ioCount::frames] == 1);
        CHECK(stats.of(node)[ioCount::malformed] == 1);
    }
    CHECK(reported(stats).empty());
}

TEST_CASE("the serial bridge writes serialOut frames as lines") {
    pty board;
    primitives::ioHub hub;
    const auto out = hub.openSerialOut("led", board.path(), 9600);
    ioStats stats;
    const serialBridge bridge(hub, stats, fast());
    REQUIRE(out->queue.push(frameOf({1.0f, 0.5f})));
    REQUIRE(out->queue.push(frameOf({-2.0f})));
    std::string written;
    CHECK(support::waitUntil([&] {
        written += board.read();
        return written.size() >= 9;
    }));
    CHECK(written == "1 0.5\n-2\n");
    CHECK(stats.of("led")[ioCount::frames] == 2);
}

TEST_CASE("a serial device that goes away counts a disconnect and opens again when it returns (io.md 4.6)") {
    const support::tempDir dir;
    auto first = std::make_unique<pty>();
    const std::string link = (dir / "board").string();
    std::filesystem::create_symlink(first->path(), link);
    primitives::ioHub hub;
    const auto in = hub.openSerialIn("knob", link, 115200);
    in->setActive(true);
    ioStats stats;
    const serialBridge bridge(hub, stats, fast());
    first->write("1\n");
    REQUIRE(next(*in));

    // The same name returns on another terminal, then the first hangs up.
    pty second;
    std::filesystem::remove(link);
    std::filesystem::create_symlink(second.path(), link);
    first->close();
    CHECK(support::waitUntil([&] { return stats.of("knob")[ioCount::disconnects] == 1; }));
    CHECK(reported(stats) == "knob: serial device " + link + " disconnected; waiting for it to return (1)\n");
    CHECK(stats.of("knob")[ioCount::disconnects] == 1);
    second.write("2\n");
    CHECK(valuesOf(support::unwrap(next(*in))) == std::vector<float>{2.0f});
    CHECK(stats.of("knob")[ioCount::disconnects] == 1);
}

TEST_CASE("the serial bridge counts one error for a pattern with no device and drops output for it") {
    primitives::ioHub hub;
    const auto out = hub.openSerialOut("led", "/nonexistent/espresso-board", 115200);
    ioStats stats;
    const serialBridge bridge(hub, stats, fast());
    REQUIRE(out->queue.push(frameOf({1.0f})));
    CHECK(support::waitUntil([&] { return stats.of("led")[ioCount::drops] == 1; }));
    CHECK(hub.drops() == 1);
    CHECK(stats.of("led")[ioCount::drops] == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));  // a few rescans
    CHECK(reported(stats) ==
          "led: no serial device matches /nonexistent/espresso-board; waiting for one (1)\n");
}

TEST_CASE("the serial bridge reports a path that is not a serial device") {
    const support::tempDir dir;
    std::ofstream(dir / "file") << "";
    primitives::ioHub hub;
    const auto in = hub.openSerialIn("knob", (dir / "file").string(), 115200);
    in->setActive(true);
    ioStats stats;
    const serialBridge bridge(hub, stats, fast());
    CHECK(support::waitUntil([&] { return stats.errors() == 1; }));
    CHECK(reported(stats) ==
          "knob: cannot open serial device " + (dir / "file").string() + ": not a serial device (1)\n");
}
