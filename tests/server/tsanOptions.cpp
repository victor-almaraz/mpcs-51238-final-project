// ThreadSanitizer settings for the server suite.
//
// On macOS, curl_global_init() calls SCDynamicStoreCopyProxies() (curl's
// CURL_MACOS_CALL_COPYPROXIES), which leaves idle libdispatch threads that
// nothing can join. After the libcurl test, the suite's forked tests
// (signals in apps_test) fork a process that has other threads, and
// ThreadSanitizer kills a child that then starts one: "starting new threads
// after multi-threaded fork is not supported". The children only start a
// signal thread and exit, so the suite lets them (lessons.md).

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define ESPRESSO_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define ESPRESSO_TSAN 1
#endif

#ifdef ESPRESSO_TSAN
// NOLINTNEXTLINE(readability-identifier-naming): ThreadSanitizer looks this function up by name
extern "C" const char* __tsan_default_options() { return "die_after_fork=0"; }
#endif
