#include "realtime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

#if defined(__APPLE__)
#include <pthread.h>

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#elif defined(__linux__)
#include <cstring>

#include <pthread.h>

#include <sched.h>
#endif

namespace server {

#if defined(__APPLE__)

std::string makeCurrentThreadRealtime(std::chrono::nanoseconds period, std::chrono::nanoseconds computation) {
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0) return "no Mach timebase";
    // Mach policies count in absolute time units.
    const auto units = [&](std::chrono::nanoseconds ns) {
        return static_cast<std::uint32_t>(ns.count() * timebase.denom / timebase.numer);
    };
    thread_time_constraint_policy_data_t policy{.period = units(period),
                                                .computation = units(std::min(computation, period)),
                                                .constraint = units(period),
                                                .preemptible = TRUE};
    // The Mach interface takes the policy as an untyped integer array.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Mach policy data, read as integers
    const auto data = reinterpret_cast<thread_policy_t>(&policy);
    const kern_return_t result =
        thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, data,
                          THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    return result == KERN_SUCCESS ? std::string() : std::string(mach_error_string(result));
}

#elif defined(__linux__)

std::string makeCurrentThreadRealtime(std::chrono::nanoseconds, std::chrono::nanoseconds) {
    // Below the priorities PipeWire gives its own threads, above every
    // ordinary thread. SCHED_FIFO needs an rtprio limit or CAP_SYS_NICE.
    sched_param param{};
    param.sched_priority = std::min(70, sched_get_priority_max(SCHED_FIFO));
    const int error = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    return error == 0 ? std::string() : std::string(std::strerror(error));
}

#else

std::string makeCurrentThreadRealtime(std::chrono::nanoseconds, std::chrono::nanoseconds) {
    return "not supported on this system";
}

#endif

}  // namespace server
