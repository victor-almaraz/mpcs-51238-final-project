// espresso::jthread: a thread that is asked to stop and joined when destroyed.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

#include <espresso/protocol/jthread.h>

TEST_CASE("a jthread runs its function until asked to stop, and moves with its thread") {
    std::atomic<bool> running{false};
    espresso::jthread t([&](const espresso::stop_token& stop) {
        running = true;
        while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    CHECK(t.joinable());
    CHECK_FALSE(t.get_stop_token().stop_requested());
    espresso::jthread moved(std::move(t));
    CHECK(moved.joinable());
    CHECK_FALSE(
        t.joinable());  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move): moved-from is empty
    CHECK(moved.request_stop());
    CHECK(moved.get_stop_token().stop_requested());
    moved.join();
    CHECK_FALSE(moved.joinable());
    CHECK(running);
}

TEST_CASE("a default stop token and jthread hold no thread") {
    const espresso::stop_token token;
    CHECK_FALSE(token.stop_requested());
    const espresso::jthread none;
    CHECK_FALSE(none.joinable());
}
