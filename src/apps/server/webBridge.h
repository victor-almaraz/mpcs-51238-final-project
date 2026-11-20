// The web bridge (io.md §5–§7): polls the URLs webIn nodes read, holds
// open the streams and sockets of webStream and webSocket nodes, and sends
// the requests webOut nodes queue. Its rules are JUCE-free and run against
// any httpTransport.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/primitives/io.h>

#include "ioStats.h"
#include "webData.h"
#include "webTransport.h"

namespace server {

/// The request a webOut frame sends (io.md §7.2): a JSON object of the
/// request's fields and the frame's values, `{"temp": 21.5}`, as
/// `application/json`; with `get`, the same pairs URL-encoded in the
/// query string. Values past the fields have no key and are left out; a
/// frame of one value with no fields sends it as `value`.
httpRequest outRequest(const primitives::webRequest& request, const primitives::dataFrame& frame);

/// What the bridge has counted, over every node (io.md §10.1).
struct webCounts {
    std::uint64_t polls = 0;      ///< Poll requests started
    std::uint64_t late = 0;       ///< Polls skipped because the last was still running
    std::uint64_t frames = 0;     ///< Frames delivered
    std::uint64_t missing = 0;    ///< Fields missing from documents read
    std::uint64_t malformed = 0;  ///< Stream and socket messages that were not JSON, or too large
    std::uint64_t skipped = 0;    ///< Binary socket messages
    std::uint64_t sent = 0;       ///< webOut requests started
    std::uint64_t errors = 0;     ///< Failed requests, streams, and sockets
};

/// Settings for a web bridge.
struct webOptions {
    /// The device clock, in seconds, that stamps each frame delivered.
    std::function<double()> clock;
    /// The time the bridge schedules by; steady_clock by default, a fake
    /// one in tests.
    std::function<std::chrono::steady_clock::time_point()> now = [] {
        return std::chrono::steady_clock::now();
    };
};

/// Moves numbers between web endpoints and the hub's web ports. Not
/// thread-safe: one thread calls step(), and its transport calls back on
/// that thread.
class webBridge {
public:
    /// Moves data between @p io's web ports and @p transport, counting
    /// what each node receives, sends, and loses, and each error, in
    /// @p stats (io.md §10).
    webBridge(primitives::ioHub& io, httpTransport& transport, ioStats& stats, webOptions options);
    ~webBridge();

    webBridge(const webBridge&) = delete;
    webBridge& operator=(const webBridge&) = delete;
    webBridge(webBridge&&) = delete;
    webBridge& operator=(webBridge&&) = delete;

    /// One round: follows the active ports, starts the polls that are due
    /// and the streams and sockets that are missing, sends queued webOut
    /// requests, and lets the transport work for at most @p wait.
    void step(std::chrono::milliseconds wait);

    /// What the bridge has counted so far; any thread may read it.
    webCounts counts() const noexcept;

private:
    struct poll;
    struct stream;
    struct socket;

    void schedulePolls(std::chrono::steady_clock::time_point now);
    void followStreams(std::chrono::steady_clock::time_point now);
    void followSockets(std::chrono::steady_clock::time_point now);
    void sendOutput();
    // Delivers @p document read at each port's fields to the active ports
    // of @p kind that @p reads chooses; false when it is not JSON or too
    // large.
    bool deliver(primitives::portKind kind, std::string_view document,
                 const std::function<bool(const primitives::webPort&)>& reads);
    // The active nodes of @p kind whose requests share @p key.
    std::vector<std::string> nodesOf(primitives::portKind kind, const std::string& key) const;
    void fail(const std::vector<std::string>& nodes, const std::string& message);
    // Counts @p n messages that did not read for the nodes of @p key.
    void malformed(primitives::portKind kind, const std::string& key, std::uint64_t n);

    primitives::ioHub& io_;
    httpTransport& transport_;
    ioStats& stats_;
    webOptions options_;
    std::map<std::string, std::unique_ptr<poll>> polls_;      // by URL, headers, and interval
    std::map<std::string, std::unique_ptr<stream>> streams_;  // by URL and headers
    std::map<std::string, std::unique_ptr<socket>> sockets_;  // by URL, headers, and hello
    struct atomicCounts {
        std::atomic<std::uint64_t> polls{0};
        std::atomic<std::uint64_t> late{0};
        std::atomic<std::uint64_t> frames{0};
        std::atomic<std::uint64_t> missing{0};
        std::atomic<std::uint64_t> malformed{0};
        std::atomic<std::uint64_t> skipped{0};
        std::atomic<std::uint64_t> sent{0};
        std::atomic<std::uint64_t> errors{0};
    } counts_;
};

}  // namespace server
