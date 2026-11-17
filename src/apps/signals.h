// SIGINT and SIGTERM handling for the servers, on POSIX signals only (no
// JUCE), so both servers stop the same way (lessons.md, "Background jobs
// start with SIGINT ignored").

#pragma once

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <system_error>

#include <pthread.h>

#include <espresso/protocol/jthread.h>

namespace apps {

// The POSIX signal API takes sets by pointer and dispositions as function
// pointers (SIG_DFL): the narrow unsafe uses, confined to signalMask (see
// the modern-cpp skill).

// Restores the default disposition of SIGINT and SIGTERM and blocks them
// in the calling thread, and in every thread it creates afterwards, for
// its lifetime. A shell starts background jobs with SIGINT ignored, and an
// ignored signal is discarded, not queued.
class signalMask {
public:
    // Throws std::system_error when a disposition or the mask cannot be
    // set: the servers would then not stop on SIGINT or SIGTERM.
    signalMask() {
        for (const int sig : {SIGINT, SIGTERM}) {
            if (std::signal(sig, SIG_DFL) == SIG_ERR)
                throw std::system_error(errno, std::generic_category(),
                                        "cannot restore a signal's default handling");
        }
        sigemptyset(&set_);
        sigaddset(&set_, SIGINT);
        sigaddset(&set_, SIGTERM);
        if (const int error = pthread_sigmask(SIG_BLOCK, &set_, &previous_); error != 0)
            throw std::system_error(error, std::generic_category(), "cannot block SIGINT and SIGTERM");
    }
    // Restores the calling thread's previous mask; a signal still pending
    // is then delivered with its default action.
    ~signalMask() { pthread_sigmask(SIG_SETMASK, &previous_, nullptr); }

    signalMask(const signalMask&) = delete;
    signalMask& operator=(const signalMask&) = delete;

    // Waits for SIGINT or SIGTERM; returns false when interrupted.
    bool wait() const noexcept {
        int sig = 0;
        return sigwait(&set_, &sig) == 0;
    }

    // Sends SIGTERM to @p thread, which wakes a wait() there. SIGTERM is
    // blocked in every thread, so it does not terminate anything: sigwait
    // in the waiter's thread receives it.
    static void wake(espresso::jthread& thread) {
        // NOLINTNEXTLINE(bugprone-bad-signal-to-kill-thread): wakes sigwait; SIGTERM is blocked here
        pthread_kill(thread.native_handle(), SIGTERM);
    }

private:
    sigset_t set_{};
    sigset_t previous_{};
};

// Waits for SIGINT and SIGTERM on its own thread. Construct it before any
// other thread starts, so every thread blocks them.
//
// The first signal sets requested(), so the main loop shuts down in
// order. A second signal before the waiter is destroyed means shutdown is
// stuck or the user insists: the process exits at once with exitStatus.
// On destruction the waiter wakes its thread with SIGTERM, which then sees
// the stop request and returns; it joins the thread, then unblocks the
// signals.
class signalWaiter {
public:
    signalWaiter()
        : thread_([this](const espresso::stop_token& stop) {
              while (!stop.stop_requested()) {
                  if (!mask_.wait()) continue;        // interrupted: wait again
                  if (stop.stop_requested()) return;  // the wake-up from the destructor
                  if (requested_.exchange(true)) std::_Exit(exitStatus);
              }
          }) {}

    ~signalWaiter() {
        thread_.request_stop();
        signalMask::wake(thread_);
    }

    signalWaiter(const signalWaiter&) = delete;
    signalWaiter& operator=(const signalWaiter&) = delete;

    // True after the first SIGINT or SIGTERM.
    bool requested() const noexcept { return requested_.load(); }

    // The exit status after a second signal: 128 + SIGINT, as a shell
    // reports an interrupted program.
    static constexpr int exitStatus = 130;

private:
    signalMask mask_;  // first: blocks before the thread starts, unblocks after it joins
    std::atomic<bool> requested_{false};
    espresso::jthread thread_;  // last: joins before the mask is restored
};

}  // namespace apps
