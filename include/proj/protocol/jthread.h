/// @file
/// `proj::jthread`: a thread that asks itself to stop and joins when it is
/// destroyed, as C++20's `std::jthread`. Standard libraries that provide
/// `std::jthread` use it; others (libc++ before version 18, including Apple
/// clang 15) get the subset below with the same interface. Header-only. It
/// lives in `protocol`, the lowest module that every component with a
/// thread links.

#pragma once

#include <thread>
#include <version>

#if defined(__cpp_lib_jthread)

#include <stop_token>

namespace proj {

/// The standard joining thread.
using std::jthread;
/// The standard stop token.
using std::stop_token;

} // namespace proj

#else

#include <atomic>
#include <memory>
#include <type_traits>
#include <utility>

namespace proj {

/// Tells a thread function that its thread was asked to stop.
class stop_token {
public:
    /// A token that never reports a stop request.
    stop_token() noexcept = default;

    /// True once the owning jthread was asked to stop.
    bool stop_requested() const noexcept { return flag_ && flag_->load(std::memory_order_acquire); }

private:
    friend class jthread;
    explicit stop_token(std::shared_ptr<const std::atomic<bool>> flag) noexcept : flag_(std::move(flag)) {}

    std::shared_ptr<const std::atomic<bool>> flag_;
};

/// A thread that requests a stop and joins on destruction.
class jthread {
public:
    /// No thread.
    jthread() noexcept = default;

    /// Runs @p f on a new thread. @p f may take a stop_token as its only
    /// argument.
    template <typename F>
    explicit jthread(F&& f) : flag_(std::make_shared<std::atomic<bool>>(false)) {
        stop_token token(flag_);
        thread_ = std::thread([fn = std::forward<F>(f), token]() mutable {
            if constexpr (std::is_invocable_v<F&, stop_token>) {
                fn(token);
            } else {
                fn();
            }
        });
    }

    /// Requests a stop and joins.
    ~jthread() { stopAndJoin(); }

    /// Takes over @p other's thread.
    jthread(jthread&& other) noexcept = default;
    /// Stops and joins this thread, then takes over @p other's.
    jthread& operator=(jthread&& other) noexcept {
        if (this != &other) {
            stopAndJoin();
            flag_ = std::move(other.flag_);
            thread_ = std::move(other.thread_);
        }
        return *this;
    }
    jthread(const jthread&) = delete;
    jthread& operator=(const jthread&) = delete;

    /// Asks the thread to stop. Returns false when there is no thread or a
    /// stop was already requested.
    bool request_stop() noexcept { return flag_ && !flag_->exchange(true, std::memory_order_acq_rel); }

    /// The token the thread function receives.
    stop_token get_stop_token() const noexcept { return stop_token(flag_); }

    /// True while the thread can be joined.
    bool joinable() const noexcept { return thread_.joinable(); }
    /// Waits for the thread to finish.
    void join() { thread_.join(); }
    /// The platform's thread handle.
    std::thread::native_handle_type native_handle() { return thread_.native_handle(); }

private:
    void stopAndJoin() noexcept {
        if (!thread_.joinable()) return;
        request_stop();
        thread_.join();
    }

    std::shared_ptr<std::atomic<bool>> flag_;
    std::thread thread_;
};

} // namespace proj

#endif
