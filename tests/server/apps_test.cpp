// Helpers shared by the executables: number parsing, signal handling, and
// the device block.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <espresso/primitives/engine.h>
#include <espresso/primitives/primitive.h>
#include <espresso/protocol/jthread.h>

#include "../arguments.h"
#include "../signals.h"
#include "deviceBlock.h"
#include "realtime.h"
#include "support/timing.h"

TEST_CASE("numbers parse only when the whole text is a finite number") {
    using apps::parseNumber;
    CHECK(parseNumber<double>("80") == 80.0);
    CHECK(parseNumber<double>("-1.5e3") == -1500.0);
    CHECK(parseNumber<double>(".5") == 0.5);
    for (const std::string_view bad :
         {"nan", "inf", "-inf", "NaN", "1e999", "0x10", " 80", "80 ", "", ".", "1e", "5abc"}) {
        INFO(bad);
        CHECK_FALSE(parseNumber<double>(bad));
    }
    CHECK(parseNumber<int>("256") == 256);
    CHECK_FALSE(parseNumber<int>("256.5"));
    CHECK_FALSE(parseNumber<int>("1e3"));
    CHECK_FALSE(parseNumber<std::uint64_t>("-1"));
    CHECK(parseNumber<std::uint64_t>("18446744073709551615") == std::numeric_limits<std::uint64_t>::max());
    // A view into a longer string ends where the view ends.
    const std::string text = "4096x";
    CHECK(parseNumber<int>(std::string_view(text).substr(0, 4)) == 4096);
}

TEST_CASE("ranges and ports refuse values outside them") {
    CHECK(apps::parseInRange<double>("48000", 8000.0, 384000.0) == 48000.0);
    CHECK_FALSE(apps::parseInRange<double>("nan", 8000.0, 384000.0));
    CHECK_FALSE(apps::parseInRange<double>("7999.9", 8000.0, 384000.0));
    CHECK(apps::parseInRange<int>("64", 0, 64) == 64);
    CHECK_FALSE(apps::parseInRange<int>("65", 0, 64));
    CHECK(apps::parsePort("57190") == std::uint16_t{57190});
    for (const std::string_view bad : {"0", "65536", "80.5", "nan", "-1", "http"}) {
        INFO(bad);
        CHECK_FALSE(apps::parsePort(bad));
    }
}

namespace {

// Runs @p body in a child process and returns its exit status, or -1 when
// it did not exit normally. The child has one thread, as a server has
// before its signal waiter starts.
template <typename F>
int inChild(F body) {
    const auto pid = fork();
    if (pid == 0) {
        body();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Waits for @p signals to see a signal.
bool awaitRequest(const apps::signalWaiter& signals) {
    return support::waitUntil([&] { return signals.requested(); });
}

}  // namespace

// In this process, so its coverage counts: CTest runs each test case in
// a process of its own, and every thread blocks the signal, so the
// waiter's sigwait takes it.
TEST_CASE("the first signal requests a shutdown; the waiter then stops cleanly") {
    {
        const apps::signalWaiter signals;
        CHECK_FALSE(signals.requested());
        REQUIRE(kill(getpid(), SIGTERM) == 0);
        CHECK(awaitRequest(signals));
    }
    // The waiter woke, joined, and unblocked the signals.
}

TEST_CASE("a signal waiter that saw no signal stops when destroyed") {
    const apps::signalWaiter signals;
    CHECK_FALSE(signals.requested());
}

TEST_CASE("a second signal before shutdown completes exits at once with status 130") {
    CHECK(inChild([] {
              const apps::signalWaiter signals;
              kill(getpid(), SIGINT);
              if (!awaitRequest(signals)) _exit(3);
              kill(getpid(), SIGINT);
              std::this_thread::sleep_for(std::chrono::seconds(2));  // a stuck shutdown
              _exit(4);
          }) == apps::signalWaiter::exitStatus);
}

TEST_CASE("device output channels beyond the engine's are silenced") {
    primitives::engine engine({.audio = {.sampleRate = 48000.0, .blockSize = 64}, .pipes = {}});
    constexpr std::size_t frames = 64;
    constexpr auto channels = static_cast<std::size_t>(primitives::maxDeviceChannels) + 2;
    std::vector<std::vector<float>> buffers(channels, std::vector<float>(frames, 1.0f));
    // The driver's channel array, as JUCE passes it; the last channel is absent.
    std::vector<float*> out;
    out.reserve(buffers.size());
    for (auto& b : buffers) out.push_back(b.data());
    out.back() = nullptr;
    server::runDeviceBlock(engine, {}, std::span<float* const>(out), frames, 0.0);
    for (std::size_t c = 0; c + 1 < channels; ++c) {
        INFO("channel " << c);
        CHECK(std::all_of(buffers[c].begin(), buffers[c].end(), [](float v) { return v == 0.0f; }));
    }
    CHECK(buffers.back()[0] == 1.0f);  // never written through a null channel
}

TEST_CASE("a device that drives no outputs only clocks the engine and stays silent") {
    primitives::engine engine({.audio = {.sampleRate = 48000.0, .blockSize = 64}, .pipes = {}});
    constexpr std::size_t frames = 64;
    std::vector<std::vector<float>> buffers(2, std::vector<float>(frames, 1.0f));
    std::vector<float*> out{buffers[0].data(), buffers[1].data()};
    const auto before = engine.beat();
    server::runDeviceBlock(engine, {}, std::span<float* const>(out), frames, 0.0, 0);
    for (const auto& b : buffers) CHECK(std::ranges::all_of(b, [](float v) { return v == 0.0f; }));
    CHECK(engine.beat() > before);  // the block ran
}

TEST_CASE("the no-audio timer's thread can ask for real-time scheduling") {
    std::string error = "not run";
    {
        const espresso::jthread thread([&](const espresso::stop_token&) {
            error = server::makeCurrentThreadRealtime(std::chrono::microseconds(5333),
                                                      std::chrono::microseconds(2666));
        });
    }
#if defined(__APPLE__)
    CHECK(error.empty());  // a time-constraint policy needs no privilege
#else
    CHECK(error != "not run");  // SCHED_FIFO may need an rtprio limit; the reason is reported
#endif
}
