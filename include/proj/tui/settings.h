/// @file
/// Editor settings: location, reading, and writing (`tui.md` §8.8,
/// §8.9, §4.2). Has no terminal dependency.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tui {

/// Key binding scheme (`tui.md` §8.2).
enum class keyScheme { emacs, cua, vim, nano };

/// Swap quantization grid (`grammar.md` §4.5).
enum class quantization { none, beat, bar };

std::string_view toString(keyScheme scheme) noexcept;
std::string_view toString(quantization quant) noexcept;

/// Editor settings with their defaults.
struct settings {
    keyScheme keys = keyScheme::emacs;
    bool fileList = true;           ///< File list visible
    bool console = true;            ///< Console visible
    double fade = 0.05;             ///< Swap fade, in seconds
    quantization quant = quantization::none;
    bool rebuildOnSave = false;
    int fileListWidth = 24;         ///< File list width, in columns
    int consoleHeight = 8;          ///< Console height, in rows
    std::string theme = "terminal"; ///< Theme name (see theme.h)

    friend bool operator==(const settings&, const settings&) = default;
};

/// Settings read from a file, and any errors found. Valid entries are
/// applied even when others are in error.
struct settingsResult {
    tui::settings values;
    std::vector<std::string> errors;
};

/// Formats a duration as milliseconds, such as "50ms", or "50 ms" when
/// @p spaced (the unit after a space, for display).
std::string formatMilliseconds(double seconds, bool spaced = false);

/// Parses a non-negative duration written as a unit literal ("50ms",
/// "0.1s", "250us") or a plain number in units of @p plainUnit seconds.
std::optional<double> parseDuration(std::string_view text, double plainUnit = 1.0);

/// Application directory name under the XDG config base.
inline constexpr std::string_view appDirectoryName = "proj";

/// Returns `<base>/<app>/settings.lisp`, where base is @p xdgConfigHome
/// when it is set, non-empty, and absolute, and `<home>/.config`
/// otherwise. Arguments may be null.
std::filesystem::path settingsPath(const char* xdgConfigHome, const char* home);

/// Returns settingsPath() for the current environment.
std::filesystem::path settingsPathFromEnvironment();

/// Parses settings from @p text. @p fileName names the file in errors.
/// The text is read as data with the interpreter's reader and is never
/// evaluated.
settingsResult parseSettings(std::string_view text, std::string_view fileName);

/// Loads settings from @p path. A missing file yields the defaults.
settingsResult loadSettings(const std::filesystem::path& path);

/// Formats @p values as a `(settings ...)` form.
std::string formatSettings(const settings& values);

/// Writes @p values to @p path atomically, creating its directory.
/// Returns an error message on failure.
std::string saveSettings(const std::filesystem::path& path, const settings& values);

} // namespace tui
