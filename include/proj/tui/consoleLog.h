/// @file
/// Console contents: compile errors with source locations, server
/// messages, and counters (`tui.md` §9.3). Has no terminal dependency.

#pragma once

#include <cstddef>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>

namespace tui {

/// A location that a console entry points to (`tui.md` §9.4).
struct sourceLocation {
    std::filesystem::path path;
    std::size_t line = 1;    ///< 1-based
    std::size_t column = 1;  ///< 1-based, in code points
};

/// Severity or origin of a console entry.
enum class consoleKind { info, error, server };

/// One console line.
struct consoleEntry {
    consoleKind kind = consoleKind::info;
    std::string text;
    std::optional<sourceLocation> location;
};

/// Bounded list of console entries, oldest first.
class consoleLog {
public:
    explicit consoleLog(std::size_t capacity = 1000) : capacity_(capacity) {}

    void add(consoleKind kind, std::string text, std::optional<sourceLocation> location = {});
    void info(std::string text) { add(consoleKind::info, std::move(text)); }
    void error(std::string text, std::optional<sourceLocation> location = {}) {
        add(consoleKind::error, std::move(text), std::move(location));
    }
    void clear() noexcept { entries_.clear(); }

    const std::deque<consoleEntry>& entries() const noexcept { return entries_; }
    std::size_t size() const noexcept { return entries_.size(); }

private:
    std::size_t capacity_;
    std::deque<consoleEntry> entries_;
};

} // namespace tui
