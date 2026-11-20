// The page bridge (io.md §12): an HTTP/1.1 listener on POSIX sockets and
// poll() that serves a project's front-end from `ui/` and carries OSC
// between its pages and the piece. No JUCE and no HTTP library.

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/protocol/jthread.h>

#include "fileHandle.h"
#include "pageMessages.h"

namespace server {

/// Settings for a page bridge.
struct pageOptions {
    /// The project root, `--root`: pages are the files of its `ui/`.
    std::filesystem::path root;
    /// The body of `/espresso.js` (io.md §12.9); empty answers 404.
    std::string helper;
    /// Receives errors the control thread does not see, such as a port
    /// that cannot be listened on.
    std::function<void(std::string_view)> report;
    /// How often a socket is pinged; one that leaves two pings unanswered
    /// is closed (io.md §12.6).
    std::chrono::milliseconds pingInterval{10000};
};

/// What the bridge has counted (io.md §10).
struct pageCounts {
    std::uint64_t received = 0;  ///< Messages from pages, delivered
    std::uint64_t sent = 0;      ///< Messages to pages, queued for a connection
    std::uint64_t dropped = 0;   ///< Messages lost to a full queue
    std::uint64_t refused = 0;   ///< Requests answered with an error status
};

/// Serves pages and carries their OSC, on its own thread. The control
/// thread chooses the port, takes what pages sent, and hands it what they
/// should receive; both queues are bounded, and a full one drops and
/// counts (io.md §12.11).
class pageBridge {
public:
    /// Most connections at once, streams and sockets included; more are
    /// refused with 503.
    static constexpr std::size_t maxConnections = 16;
    /// Messages queued for one connection; more are dropped for it alone.
    static constexpr std::size_t connectionQueue = 256;
    /// Messages from pages waiting for the control thread.
    static constexpr std::size_t inboxSize = 1024;
    /// Messages for pages waiting for the bridge's thread.
    static constexpr std::size_t outboxSize = 1024;

    explicit pageBridge(pageOptions options);
    ~pageBridge();

    pageBridge(const pageBridge&) = delete;
    pageBridge& operator=(const pageBridge&) = delete;
    pageBridge(pageBridge&&) = delete;
    pageBridge& operator=(pageBridge&&) = delete;

    /// Control thread: listens on 127.0.0.1 port @p port, the active
    /// plan's page port, closing the listener and its connections first
    /// when it is another; nothing closes them (io.md §12.2). Port 0 takes
    /// a free one, for tests.
    void listen(std::optional<std::uint16_t> port);
    /// The port listened on; 0 when none.
    std::uint16_t port() const noexcept { return port_.load(); }

    /// Control thread: the messages pages sent since the last call, in
    /// order.
    std::vector<oscBytes> takeMessages();
    /// Control thread: queues @p osc for every connected page.
    void send(std::span<const std::uint8_t> osc);

    /// What the bridge has counted so far.
    pageCounts counts() const noexcept;

private:
    struct connection;
    // A message for pages: as JSON, and as OSC for binary sockets.
    struct outgoing {
        std::string json;
        oscBytes osc;
    };

    void run(const espresso::stop_token& stop);
    void accept();
    void readFrom(connection& c);
    void respond(connection& c, const httpMessage& request);
    void serveFile(connection& c, const std::string& path);
    void post(connection& c, const httpMessage& request);
    void upgrade(connection& c, const httpMessage& request);
    void frames(connection& c, std::string_view bytes);
    void deliver(std::vector<oscBytes> messages);
    void ping(connection& c, std::chrono::steady_clock::time_point now);
    void render(connection& c);
    void distribute();
    void wake() const noexcept;
    bool allowedHost(std::string_view host) const;

    pageOptions options_;
    fileHandle wakeRead_;
    fileHandle wakeWrite_;
    std::atomic<std::uint16_t> port_{0};
    std::optional<std::uint16_t> wanted_;  // control thread: the port asked for

    mutable std::mutex mutex_;
    fileHandle pendingListener_;  // a listener the bridge's thread has not taken yet
    bool listenerChanged_ = false;
    std::vector<oscBytes> inbox_;
    std::deque<std::shared_ptr<const outgoing>> outbox_;  // messages for pages

    // The bridge's thread only.
    fileHandle listener_;
    std::map<int, std::unique_ptr<connection>> connections_;  // by descriptor

    struct atomicCounts {
        std::atomic<std::uint64_t> received{0};
        std::atomic<std::uint64_t> sent{0};
        std::atomic<std::uint64_t> dropped{0};
        std::atomic<std::uint64_t> refused{0};
    } counts_;
    espresso::jthread thread_;  // last: stops and joins before the rest is destroyed
};

}  // namespace server
