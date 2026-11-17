/// @file
/// Reading and atomic writing of source files (`tui.md` §7).

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tui {

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
    /// flushes it, and renames it over @p path. On failure the original
    /// file is unchanged and the returned message describes the error
    /// (`tui.md` §7.2).
    static std::optional<std::string> writeAtomic(const std::filesystem::path& path, const std::string& text);

    /// Lists regular files under @p root, sorted, relative to it.
    /// Hidden entries (names starting with `.`) and `build` directories
    /// are skipped.
    static std::vector<std::filesystem::path> listProject(const std::filesystem::path& root);
};

}  // namespace tui
