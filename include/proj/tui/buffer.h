/// @file
/// Text buffer for one file: gap-buffer storage, cursor, selection,
/// edits, and bounded undo history (`tui.md` §5, §8.7).

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tui {

/// A position in a buffer. Both fields are 0-based; the column counts
/// code points.
struct position {
    std::size_t line = 0;
    std::size_t column = 0;

    friend bool operator==(const position&, const position&) = default;
    friend auto operator<=>(const position&, const position&) = default;
};

/// Byte storage with a movable gap, so that edits at the cursor are
/// cheap (`tui.md` §5.1).
class gapBuffer {
public:
    gapBuffer() = default;
    explicit gapBuffer(std::string_view text);

    /// Number of bytes stored.
    std::size_t size() const noexcept { return data_.size() - gapLength(); }

    /// Byte at @p offset. Requires offset < size().
    char at(std::size_t offset) const noexcept;

    /// Copy of @p length bytes starting at @p offset, clamped to size().
    std::string substr(std::size_t offset, std::size_t length) const;

    /// Copy of the whole contents.
    std::string str() const { return substr(0, size()); }

    /// Inserts @p text at @p offset.
    void insert(std::size_t offset, std::string_view text);

    /// Removes @p length bytes starting at @p offset.
    void erase(std::size_t offset, std::size_t length);

private:
    std::size_t gapLength() const noexcept { return gapEnd_ - gapStart_; }
    void moveGap(std::size_t offset);
    void reserveGap(std::size_t needed);

    std::vector<char> data_;
    std::size_t gapStart_ = 0;
    std::size_t gapEnd_ = 0;
};

/// Undo history limits, in bytes of recorded text. The defaults match
/// GNU Emacs's `undo-limit` and `undo-strong-limit` (`tui.md` §8.7).
struct undoLimits {
    std::size_t soft = 160'000;
    std::size_t hard = 240'000;
};

/// One file's text and edit state. Has no terminal dependency
/// (`tui.md` §3.1, §5.4).
class buffer {
public:
    buffer() = default;

    /// Creates a buffer holding @p text, associated with @p path.
    explicit buffer(std::string_view text, std::filesystem::path path = {},
                    undoLimits limits = {});

    /// @name Content
    /// @{
    std::string text() const { return text_.str(); }
    /// Counter incremented by every change to the text.
    std::uint64_t version() const noexcept { return version_; }
    std::size_t byteSize() const noexcept { return text_.size(); }
    std::size_t lineCount() const;
    /// Text of line @p line, without its line break.
    std::string line(std::size_t line) const;
    /// Length of line @p line in code points.
    std::size_t lineLength(std::size_t line) const;
    /// Byte offset of @p pos, after clamping it to the buffer.
    std::size_t offsetOf(position pos) const;
    /// Position of byte @p offset.
    position positionOf(std::size_t offset) const;
    /// Clamps @p pos to an existing line and column.
    position clamp(position pos) const;
    /// @}

    /// @name Cursor and selection
    /// @{
    position cursor() const noexcept { return cursor_; }
    std::size_t cursorOffset() const { return offsetOf(cursor_); }
    void setCursor(position pos);
    /// Sets the mark at the cursor, starting a selection.
    void setMark() { mark_ = cursor_; }
    void setMark(position pos) { mark_ = clamp(pos); }
    void clearMark() noexcept { mark_.reset(); }
    std::optional<position> mark() const noexcept { return mark_; }
    /// True when a mark is set and differs from the cursor.
    bool hasSelection() const noexcept { return mark_ && *mark_ != cursor_; }
    /// Byte range [first, second) between mark and cursor.
    std::pair<std::size_t, std::size_t> selectionRange() const;
    std::string selectedText() const;
    /// @}

    /// @name Movement
    /// Movement ends any run of typed characters for undo grouping.
    /// @{
    void moveLeft();
    void moveRight();
    void moveUp(std::size_t lines = 1);
    void moveDown(std::size_t lines = 1);
    void moveLineStart();
    void moveLineEnd();
    void moveWordForward();
    void moveWordBackward();
    void moveBufferStart();
    void moveBufferEnd();
    /// @}

    /// @name Edits
    /// Each edit sets the dirty flag (`tui.md` §5.3). Edits that would
    /// change nothing return false and leave the buffer untouched.
    /// @{
    /// Inserts @p text at the cursor, replacing the selection if any.
    void insert(std::string_view text);
    void insertLineBreak() { insert("\n"); }
    bool deleteBackward();
    bool deleteForward();
    /// Joins the cursor's line with the next line.
    bool joinLines();
    /// Deletes the selection and returns its text.
    std::string deleteSelection();
    /// Deletes from the cursor to the end of the line, or the line break
    /// when the cursor is at the end. Returns the deleted text.
    std::string killLine();
    /// Replaces @p length bytes at @p offset with @p text.
    void replace(std::size_t offset, std::size_t length, std::string_view text);
    /// @}

    /// @name Undo
    /// @{
    bool undo();
    bool redo();
    bool canUndo() const noexcept { return !undo_.empty(); }
    bool canRedo() const noexcept { return !redo_.empty(); }
    /// Bytes currently held by the undo history.
    std::size_t undoBytes() const noexcept { return undoBytes_; }
    std::size_t undoDepth() const noexcept { return undo_.size(); }
    /// Ends the current group of typed characters.
    void breakUndoGroup() noexcept { mergeable_ = false; }
    /// @}

    /// @name File state
    /// @{
    const std::filesystem::path& path() const noexcept { return path_; }
    void setPath(std::filesystem::path path) { path_ = std::move(path); }
    bool dirty() const noexcept { return dirty_; }
    void markClean() noexcept { dirty_ = false; }
    /// @}

    /// @name View state
    /// @{
    std::size_t topLine() const noexcept { return topLine_; }
    void setTopLine(std::size_t line) noexcept { topLine_ = line; }
    std::size_t leftColumn() const noexcept { return leftColumn_; }
    void setLeftColumn(std::size_t column) noexcept { leftColumn_ = column; }
    /// Adjusts topLine so the cursor is visible in @p rows rows.
    void scrollToCursor(std::size_t rows);
    /// @}

private:
    struct edit {
        std::size_t offset;
        std::string removed;
        std::string inserted;
        position before;
        position after;
        std::size_t bytes() const noexcept { return removed.size() + inserted.size(); }
    };

    void apply(std::size_t offset, std::size_t length, std::string_view text);
    void record(edit e, bool mergeableInsert);
    void trimUndo();
    void endMovement() noexcept;
    const std::vector<std::size_t>& lineStarts() const;
    bool isWordByte(std::size_t offset) const noexcept;

    gapBuffer text_;
    std::filesystem::path path_;
    undoLimits limits_;
    position cursor_;
    std::optional<position> mark_;
    std::optional<std::size_t> goalColumn_;
    std::size_t topLine_ = 0;
    std::size_t leftColumn_ = 0;
    std::uint64_t version_ = 0;
    bool dirty_ = false;

    std::deque<edit> undo_;  // oldest at front
    std::vector<edit> redo_;
    std::size_t undoBytes_ = 0;
    bool mergeable_ = false;

    mutable std::vector<std::size_t> lineStarts_;
    mutable bool linesValid_ = false;
};

} // namespace tui
