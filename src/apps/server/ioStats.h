// Per-node I/O counters and errors (io.md §10): the bridges count what
// each serial and web node receives, sends, and loses, and errors reach
// the client at most once a second per node, with how many occurred.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace server {

/// What a bridge counts for a node (io.md §10.1).
enum class ioCount : std::uint8_t {
    frames,       ///< Frames received or sent
    malformed,    ///< Lines, documents, or messages that did not read
    missing,      ///< Fields missing from documents read
    late,         ///< Polls skipped because the last was still running
    errors,       ///< Failed requests, connections, and devices
    disconnects,  ///< Devices that went away while open
    drops,        ///< Output that had nowhere to go or did not fit
};

/// A node's counts, by ioCount.
struct nodeCounts {
    std::array<std::uint64_t, 7> values{};

    /// The count of @p what.
    std::uint64_t operator[](ioCount what) const noexcept { return values[static_cast<std::size_t>(what)]; }
    /// Adds @p n to the count of @p what.
    void add(ioCount what, std::uint64_t n) noexcept { values[static_cast<std::size_t>(what)] += n; }
};

/// An error to report for a node: its latest message, and how many errors
/// occurred since the last report (io.md §10.2).
struct ioError {
    std::string node;
    std::string message;
    std::int32_t count = 0;
};

/// Counts and errors from the bridges' threads, read by the control
/// thread. Thread-safe.
class ioStats {
public:
    /// The least time between two reports for one node.
    static constexpr std::chrono::seconds reportInterval{1};

    /// Adds @p n to @p what for each of @p nodes.
    void add(std::span<const std::string> nodes, ioCount what, std::uint64_t n = 1);
    /// Adds @p n to @p what for @p node.
    void add(std::string_view node, ioCount what, std::uint64_t n = 1);
    /// Counts an error for each of @p nodes, with @p message as its latest.
    void error(std::span<const std::string> nodes, std::string_view message);
    /// Counts an error for @p node.
    void error(std::string_view node, std::string_view message);

    /// @p node's counts so far.
    nodeCounts of(std::string_view node) const;
    /// Errors counted so far, over every node: `/counters`' ioErrors.
    std::uint64_t errors() const noexcept { return errors_.load(std::memory_order_relaxed); }

    /// The errors to report at @p now: one per node with errors since its
    /// last report, unless that report was less than reportInterval ago.
    std::vector<ioError> due(std::chrono::steady_clock::time_point now);

private:
    struct entry {
        nodeCounts counts;
        std::string message;  // the latest error
        std::int32_t unreported = 0;
        std::optional<std::chrono::steady_clock::time_point> reported;
    };

    entry& at(std::string_view node);  // under the lock

    mutable std::mutex mutex_;
    std::map<std::string, entry, std::less<>> nodes_;
    std::atomic<std::uint64_t> errors_{0};
};

}  // namespace server
