/// @file
/// Console contents: compile errors with source locations, server
/// messages, and counters (`tui.md` §9.3). Has no terminal dependency.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>

namespace tui {

/// A location that a console entry points to (`tui.md` §9.4).
struct sourceLocation {
    /// File, relative to the project root.
    std::filesystem::path path;
    std::size_t line = 1;    ///< 1-based
    std::size_t column = 1;  ///< 1-based, in code points
};

/// Severity or origin of a console entry.
enum class consoleKind : std::uint8_t { info, error, server };

/// One console line.
struct consoleEntry {
    /// Severity or origin.
    consoleKind kind = consoleKind::info;
    /// The line of text.
    std::string text;
    /// Where clicking the entry moves the cursor, if anywhere.
    std::optional<sourceLocation> location;
};

/// Bounded list of console entries, oldest first.
class consoleLog {
public:
    /// Creates a log that keeps the newest @p capacity entries.
    explicit consoleLog(std::size_t capacity = 1000) : capacity_(capacity) {}

    /// Appends an entry, dropping the oldest beyond the capacity.
    void add(consoleKind kind, std::string text, std::optional<sourceLocation> location = {});
    /// Appends an informational entry.
    void info(std::string text) { add(consoleKind::info, std::move(text)); }
    /// Appends an error, optionally pointing to its location.
    void error(std::string text, std::optional<sourceLocation> location = {}) {
        add(consoleKind::error, std::move(text), std::move(location));
    }
    /// Removes all entries.
    void clear() noexcept { entries_.clear(); }

    /// Entries, oldest first.
    const std::deque<consoleEntry>& entries() const noexcept { return entries_; }
    /// Number of entries.
    std::size_t size() const noexcept { return entries_.size(); }

private:
    std::size_t capacity_ = 1000;
    std::deque<consoleEntry> entries_;
};

} // namespace tui
