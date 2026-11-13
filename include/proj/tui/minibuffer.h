/// @file
/// One-line prompt with completion (`tui.md` §8.6). Has no terminal
/// dependency.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tui {

/// Returns the entries of @p candidates that contain @p text, those
/// starting with it first, each group in its original order.
std::vector<std::string> filterCandidates(const std::vector<std::string>& candidates,
                                          std::string_view text);

/// Longest common prefix of @p values.
std::string commonPrefix(const std::vector<std::string>& values);

/// Text entry state for the minibuffer.
class minibuffer {
public:
    /// Starts a prompt. @p candidates are offered for completion.
    void open(std::string prompt, std::string initial = {},
              std::vector<std::string> candidates = {});
    void close() noexcept { active_ = false; }

    bool active() const noexcept { return active_; }
    const std::string& prompt() const noexcept { return prompt_; }
    const std::string& text() const noexcept { return text_; }
    /// Cursor position in code points.
    std::size_t cursor() const noexcept { return cursor_; }

    void insert(std::string_view s);
    void deleteBackward();
    void deleteForward();
    void moveLeft();
    void moveRight();
    void moveStart() noexcept { cursor_ = 0; }
    void moveEnd();
    void setText(std::string text);

    /// Candidates matching the current text.
    std::vector<std::string> matches() const { return filterCandidates(candidates_, text_); }

    /// Extends the text to the longest common prefix of the matches that
    /// start with it. Returns true when the text changed.
    bool complete();

private:
    bool active_ = false;
    std::string prompt_;
    std::string text_;
    std::size_t cursor_ = 0;
    std::vector<std::string> candidates_;
};

} // namespace tui
