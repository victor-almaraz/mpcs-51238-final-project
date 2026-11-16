/// @file
/// `proj::jthread`: a thread that asks itself to stop and joins when it is
/// destroyed, with the interface of C++20's `std::jthread` that the
/// project uses. Unlike `std::jthread`, every thread gets a stack of
/// `threadStackBytes`: secondary-thread stacks otherwise differ by
/// platform (512 KiB on macOS; RLIMIT_STACK, or 2 MiB when unlimited, with
/// glibc), and the interpreter's worker needs a measured size
/// (`interpreter::maxEvalDepth`). Header-only. It lives in `protocol`, the
/// lowest module that every component with a thread links.

#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <system_error>
#include <type_traits>
#include <utility>

#include <pthread.h>

/// Project-wide utilities that the standard library lacks on every target.
namespace proj {

/// Stack size of every proj::jthread: room for the interpreter's deepest
/// evaluation (`interpreter::maxEvalDepth` levels at about 1.3 KiB each in
/// a Debug build) with a wide margin for sanitizer builds.
inline constexpr std::size_t threadStackBytes = std::size_t{8} << 20u;

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

    /// Runs @p f on a new thread with a stack of threadStackBytes. @p f may
    /// take a stop_token as its only argument. Throws std::system_error if
    /// the thread cannot be created.
    template <typename F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, jthread>)  // as std::jthread: never a copy
    explicit jthread(F&& f) : flag_(std::make_shared<std::atomic<bool>>(false)) {
        body_ = std::make_unique<std::function<void()>>(
            [fn = std::forward<F>(f), token = stop_token(flag_)]() mutable {
                // fn is a non-const copy and is called as an lvalue.
                if constexpr (std::is_invocable_v<std::decay_t<F>&, stop_token&>) {
                    fn(token);
                } else {
                    fn();
                }
            });
        start();
    }

    /// Requests a stop and joins.
    ~jthread() { stopAndJoin(); }

    /// Takes over @p other's thread.
    jthread(jthread&& other) noexcept
        : flag_(std::move(other.flag_)),
          body_(std::move(other.body_)),
          thread_(other.thread_),
          joinable_(std::exchange(other.joinable_, false)) {}
    /// Stops and joins this thread, then takes over @p other's.
    jthread& operator=(jthread&& other) noexcept {
        if (this != &other) {
            stopAndJoin();
            flag_ = std::move(other.flag_);
            body_ = std::move(other.body_);
            thread_ = other.thread_;
            joinable_ = std::exchange(other.joinable_, false);
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
    bool joinable() const noexcept { return joinable_; }
    /// Waits for the thread to finish. Throws std::system_error if there is
    /// no thread to join.
    void join() {
        if (!joinable_) throw std::system_error(std::make_error_code(std::errc::invalid_argument), "join");
        const int result = ::pthread_join(thread_, nullptr);  // no result wanted
        joinable_ = false;
        if (result != 0) throw std::system_error(result, std::generic_category(), "pthread_join");
    }
    /// The platform's thread handle.
    pthread_t native_handle() const noexcept { return thread_; }

private:
    // POSIX threads pass the start routine's argument as void*: the narrow
    // unsafe use at this boundary (modern-cpp skill, section 9). It is
    // body_, which this object owns until the thread is joined.
    static void* run(void* body) {
        (*static_cast<std::function<void()>*>(body))();
        return nullptr;
    }

    void start() {
        pthread_attr_t attributes;
        if (const int r = ::pthread_attr_init(&attributes); r != 0) {
            throw std::system_error(r, std::generic_category(), "pthread_attr_init");
        }
        int result = ::pthread_attr_setstacksize(&attributes, threadStackBytes);
        if (result == 0) result = ::pthread_create(&thread_, &attributes, &jthread::run, body_.get());
        ::pthread_attr_destroy(&attributes);
        if (result != 0) throw std::system_error(result, std::generic_category(), "pthread_create");
        joinable_ = true;
    }

    void stopAndJoin() noexcept {
        if (!joinable_) return;
        request_stop();
        ::pthread_join(thread_, nullptr);
        joinable_ = false;
    }

    std::shared_ptr<std::atomic<bool>> flag_;
    std::unique_ptr<std::function<void()>> body_;  // heap-held, so its address survives moves of this object
    pthread_t thread_{};
    bool joinable_ = false;
};

} // namespace proj
