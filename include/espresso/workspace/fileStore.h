/// @file
/// Reading and atomic writing of source files (`tui.md` §7).

#pragma once

#include <cstddef>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace workspace {

/// Result of reading a file: its text, or an error message naming it.
struct readFileResult {
    /// File contents, when the read succeeded.
    std::optional<std::string> text;
    /// Error message naming the file, when it failed.
    std::string error;
};

/// Reads and writes files. Has no terminal dependency (`tui.md` §3.1).
class fileStore {
public:
    /// Reads @p path as UTF-8. A file that is not valid UTF-8 is
    /// rejected with an error naming the file (`tui.md` §7.1).
    static readFileResult read(const std::filesystem::path& path);

    /// Writes @p text to a temporary file in the target's directory,
    /// flushes it, and renames it over @p path. When @p path is a
    /// symbolic link, the file it names is replaced and the link is kept.
    /// On failure the original file is unchanged and the returned message
    /// describes the error (`tui.md` §7.2).
    static std::optional<std::string> writeAtomic(const std::filesystem::path& path, const std::string& text);

    /// Creates an empty file at @p path, with the directories above it that
    /// are missing. Fails, and changes nothing, when something by that name
    /// exists; the returned message names it (`patterns.md` §20.7).
    static std::optional<std::string> create(const std::filesystem::path& path);

    /// Renames @p from to @p to, a file or a directory, creating the
    /// directories above @p to that are missing. Fails, and changes
    /// nothing, when @p to exists or @p from does not.
    static std::optional<std::string> rename(const std::filesystem::path& from,
                                             const std::filesystem::path& to);

    /// When @p path was last written, as an opaque token that changes when
    /// the file does (its modification time, in nanoseconds since the file
    /// clock's epoch);
    /// nothing when it cannot be read. An editor keeps the token it read
    /// and writes only while the file still has it (`patterns.md` §13).
    static std::optional<std::string> modified(const std::filesystem::path& path);

    /// Lists regular files under @p root, sorted, relative to it.
    /// Hidden entries (names starting with `.`) and `build` directories
    /// are skipped. Symbolic links are listed by their own names, and
    /// linked directories are descended into, except a link to a
    /// directory that contains it, which would repeat forever.
    /// At most @p limit + 1 files are listed: the walk stops after the file
    /// past the limit, so a caller that gets @p limit + 1 knows the folder
    /// holds more.
    static std::vector<std::filesystem::path>
    listProject(const std::filesystem::path& root,
                std::size_t limit = std::numeric_limits<std::size_t>::max());
};

}  // namespace workspace
