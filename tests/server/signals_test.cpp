// SIGINT and SIGTERM handling for the servers (src/apps/signals.h). Only
// the first signal is tested: a second one ends the process at once.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <thread>

#include <unistd.h>

#include "../signals.h"

TEST_CASE("a signal waiter sees the first SIGTERM and stops its thread when destroyed") {
    const apps::signalWaiter waiter;  // blocks SIGINT and SIGTERM in this thread
    CHECK_FALSE(waiter.requested());
    // Sent to the process: every thread blocks it, so the waiter's sigwait takes it.
    REQUIRE(kill(getpid(), SIGTERM) == 0);
    for (int i = 0; i < 400 && !waiter.requested(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(waiter.requested());
}

TEST_CASE("a signal waiter that saw no signal stops when destroyed") {
    const apps::signalWaiter waiter;
    CHECK_FALSE(waiter.requested());
}
