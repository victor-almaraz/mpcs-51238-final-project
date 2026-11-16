// Helpers shared by the executables: number parsing, signal handling, the
// device block, and the stub server's engine.

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
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <proj/primitives/engine.h>
#include <proj/primitives/primitive.h>
#include <proj/protocol/irTransfer.h>
#include <proj/protocol/messages.h>
#include <proj/protocol/osc.h>
#include <proj/protocol/serverSession.h>
#include <proj/protocol/udp.h>

#include "../arguments.h"
#include "../signals.h"
#include "../stub-server/stubEngine.h"
#include "deviceBlock.h"
#include "support/unwrap.h"

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

// Waits up to a second for @p signals to see a signal.
bool awaitRequest(const apps::signalWaiter& signals) {
    for (int i = 0; i < 200 && !signals.requested(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return signals.requested();
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

TEST_CASE("the stub server consumes a plan on /play, as the server does") {
    stub::stubEngine engine(stub::clock_type::now());
    protocol::serverSession session(
        {.serverVersion = "stub", .metadataVersion = "meta-1", .sampleRate = 48000, .blockSize = 256},
        engine);
    const protocol::endpoint client{40001};
    std::int32_t id = 1;
    auto request = [&](std::string_view address, std::vector<protocol::argument> args) {
        args.insert(args.begin(), id++);
        const auto out = session.handle({.address = std::string(address), .args = std::move(args)}, client);
        REQUIRE(out.size() == 1);
        return out[0].msg;
    };
    REQUIRE(request(protocol::address::hello, {std::string("c"), std::string("meta-1")}).address ==
            protocol::address::helloOk);
    const std::string ir = "{}";
    REQUIRE(request(protocol::address::irBegin, protocol::irBeginArgs(3, ir)).address ==
            protocol::address::done);
    session.handle(protocol::chunkMessage(3, 0, ir), client);
    REQUIRE(request(protocol::address::irEnd, {std::int32_t{3}}).address == protocol::address::done);

    const std::vector<protocol::argument> play = {std::int32_t{3}, 0.0f, std::string("none")};
    CHECK(request(protocol::address::play, play).address == protocol::address::done);
    const auto again = request(protocol::address::play, play);
    CHECK(again.int32(1) == static_cast<std::int32_t>(protocol::errorCode::unknownPlan));
    const auto due = engine.tick(stub::clock_type::now());
    REQUIRE(due.size() == 1);
    CHECK(due[0].address == protocol::address::planActive);
}

TEST_CASE("the stub server refuses plan IDs in use and retires replaced swaps, as the server does") {
    stub::stubEngine engine(stub::clock_type::now());
    REQUIRE_FALSE(engine.loadPlan(3, "{}"));
    CHECK(engine.loadPlan(3, "{}"));  // loaded, not yet played
    engine.play(3, 0.0f, "bar");      // waits for the next bar
    CHECK(engine.loadPlan(3, "{}"));  // waiting to play
    REQUIRE_FALSE(engine.loadPlan(4, "{}"));
    engine.play(4, 0.0f, "none");  // replaces the pending swap of plan 3
    const auto due = engine.tick(stub::clock_type::now());
    std::vector<std::string> seen;
    seen.reserve(due.size());
    for (const auto& m : due) seen.push_back(m.address + " " + std::to_string(support::unwrap(m.int32(0))));
    CHECK(seen == std::vector<std::string>{"/plan.active 4", "/plan.retired 3"});
    CHECK_FALSE(engine.loadPlan(3, "{}"));  // free again after /plan.retired
}

TEST_CASE("the stub server stores controls, reports its active plan, stops, and quits") {
    stub::stubEngine engine(stub::clock_type::now());
    REQUIRE_FALSE(engine.loadPlan(5, "{}"));
    engine.play(5, 0.0f, "none");
    REQUIRE(engine.tick(stub::clock_type::now()).size() == 1);  // /plan.active 5
    CHECK(engine.counters().activePlan == 5);
    engine.setControl(3, 0.5f);
    CHECK(engine.control(3) == 0.5f);
    CHECK(engine.control(4) == 0.0f);  // never set
    engine.stop(0.0f);
    const auto retired = engine.tick(stub::clock_type::now());
    REQUIRE(retired.size() == 1);
    CHECK(retired[0].address == protocol::address::planRetired);
    CHECK(engine.counters().activePlan == 0);
    CHECK_FALSE(engine.quitRequested());
    engine.quit();
    CHECK(engine.quitRequested());
}
