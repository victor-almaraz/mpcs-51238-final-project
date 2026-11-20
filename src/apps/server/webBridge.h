// The web bridge (io.md §5–§7): polls the URLs webIn nodes read, holds
// open the streams and sockets of webStream and webSocket nodes, and sends
// the requests webOut nodes queue. Its rules are JUCE-free and run against
// any httpTransport.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
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

private:
    // What requests share (io.md §5.3): a connection for the same URL and
    // headers, a poll schedule for the same interval too, and a socket
    // for the same hello too.
    struct shareKey {
        std::string url;
        std::vector<std::string> headers;
        std::chrono::milliseconds every{0};  // webIn
        std::string hello;                   // webSocket

        bool operator<(const shareKey& other) const {
            return std::tie(url, headers, every, hello) <
                   std::tie(other.url, other.headers, other.every, other.hello);
        }
    };
    // A node reading a shared transfer: its name, the event it reads
    // (webStream), and its fields, parsed once a round.
    struct reader {
        std::string owner;
        std::string event;
        std::vector<fieldPointer> fields;
    };
    // What every shared transfer holds: the request it serves, the ports
    // that share it, oldest first, and their readers, and its transfer while
    // one runs.
    struct shared {
        primitives::webRequest request;
        std::vector<std::shared_ptr<const primitives::webPort>> ports;
        std::vector<reader> readers;
        std::optional<httpTransport::id> transfer;

        std::vector<std::string> nodes() const;
    };
    struct poll;
    struct stream;
    struct socket;

    static shareKey keyOf(primitives::portKind kind, const primitives::webRequest& r);
    // Keeps @p entries to one per key of the active ports of @p kind,
    // making each new one with @p make and stopping each one no port
    // wants, and reads the readers of those whose ports changed.
    template <typename Entry, typename Make>
    void follow(primitives::portKind kind, std::map<shareKey, std::unique_ptr<Entry>>& entries, Make&& make);
    void schedulePolls(std::chrono::steady_clock::time_point now);
    void followStreams(std::chrono::steady_clock::time_point now);
    void followSockets(std::chrono::steady_clock::time_point now);
    void sendOutput();
    // Delivers @p document to @p readers, each at its fields, and to the
    // readers of @p event when it is given; false when it is not JSON or
    // too large.
    bool deliver(primitives::portKind kind, std::string_view document, const std::vector<reader>& readers,
                 std::optional<std::string_view> event = std::nullopt);
    // Counts @p n messages of @p e that did not read.
    void malformed(const shared& e, std::uint64_t n);

    primitives::ioHub& io_;
    httpTransport& transport_;
    ioStats& stats_;
    webOptions options_;
    std::map<shareKey, std::unique_ptr<poll>> polls_;
    std::map<shareKey, std::unique_ptr<stream>> streams_;
    std::map<shareKey, std::unique_ptr<socket>> sockets_;
};

}  // namespace server
