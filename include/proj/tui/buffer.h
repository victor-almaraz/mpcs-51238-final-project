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
    /// Line, from 0.
    std::size_t line = 0;
    /// Column in code points, from 0.
    std::size_t column = 0;

    /// Equal when line and column are equal.
    friend bool operator==(const position&, const position&) = default;
    /// Ordered by line, then column.
    friend auto operator<=>(const position&, const position&) = default;
};

/// Byte storage with a movable gap, so that edits at the cursor are
/// cheap (`tui.md` §5.1).
class gapBuffer {
public:
    gapBuffer() = default;
    /// Creates a buffer holding @p text.
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
    // Iterator to byte @p k of the storage, gap included.
    auto it(std::size_t k) noexcept { return data_.begin() + static_cast<std::ptrdiff_t>(k); }
    auto it(std::size_t k) const noexcept { return data_.cbegin() + static_cast<std::ptrdiff_t>(k); }

    std::vector<char> data_;
    std::size_t gapStart_ = 0;
    std::size_t gapEnd_ = 0;
};

/// Undo history limits, in bytes of recorded text. The defaults match
/// GNU Emacs's `undo-limit` and `undo-strong-limit` (`tui.md` §8.7).
struct undoLimits {
    /// History is trimmed to this size after the change that crosses it.
    std::size_t soft = 160'000;
    /// History never exceeds this size, except for the most recent change.
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

    /// Copy of the whole text.
    std::string text() const { return text_.str(); }
    /// Counter incremented by every change to the text.
    std::uint64_t version() const noexcept { return version_; }
    /// Size of the text in bytes.
    std::size_t byteSize() const noexcept { return text_.size(); }
    /// Number of lines; an empty buffer has one.
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

    /// Cursor position.
    position cursor() const noexcept { return cursor_; }
    /// Byte offset of the cursor.
    std::size_t cursorOffset() const { return offsetOf(cursor_); }
    /// Moves the cursor to @p pos, clamped to the buffer.
    void setCursor(position pos);
    /// Sets the mark at the cursor, starting a selection.
    void setMark() { mark_ = cursor_; }
    /// Sets the mark at @p pos, clamped to the buffer.
    void setMark(position pos) { mark_ = clamp(pos); }
    /// Removes the mark, ending the selection.
    void clearMark() noexcept { mark_.reset(); }
    /// The mark, if set.
    std::optional<position> mark() const noexcept { return mark_; }
    /// True when a mark is set and differs from the cursor.
    bool hasSelection() const noexcept { return mark_ && *mark_ != cursor_; }
    /// Byte range [first, second) between mark and cursor.
    std::pair<std::size_t, std::size_t> selectionRange() const;
    /// Text between the mark and the cursor.
    std::string selectedText() const;
    /// @}

    /// @name Movement
    /// Movement ends any run of typed characters for undo grouping.
    /// @{

    /// Moves one code point left, across line breaks.
    void moveLeft();
    /// Moves one code point right, across line breaks.
    void moveRight();
    /// Moves up @p lines lines, keeping the goal column.
    void moveUp(std::size_t lines = 1);
    /// Moves down @p lines lines, keeping the goal column.
    void moveDown(std::size_t lines = 1);
    /// Moves to the start of the line.
    void moveLineStart();
    /// Moves to the end of the line.
    void moveLineEnd();
    /// Moves past the end of the next word.
    void moveWordForward();
    /// Moves to the start of the previous word.
    void moveWordBackward();
    /// Moves to the start of the buffer.
    void moveBufferStart();
    /// Moves to the end of the buffer.
    void moveBufferEnd();
    /// @}

    /// @name Edits
    /// Each edit sets the dirty flag (`tui.md` §5.3). Edits that would
    /// change nothing return false and leave the buffer untouched.
    /// @{

    /// Inserts @p text at the cursor, replacing the selection if any.
    void insert(std::string_view text);
    /// Inserts a line break at the cursor.
    void insertLineBreak() { insert("\n"); }
    /// Deletes the selection, or the code point before the cursor.
    bool deleteBackward();
    /// Deletes the selection, or the code point after the cursor.
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

    /// Reverts the most recent change. Returns false when there is none.
    bool undo();
    /// Reapplies the most recently undone change. Returns false when there is none.
    bool redo();
    /// True when a change can be redone.
    bool canRedo() const noexcept { return !redo_.empty(); }
    /// Bytes currently held by the undo history.
    std::size_t undoBytes() const noexcept { return undoBytes_; }
    /// Number of changes in the undo history.
    std::size_t undoDepth() const noexcept { return undo_.size(); }
    /// @}

    /// @name File state
    /// @{

    /// File path, or empty for an unnamed buffer.
    const std::filesystem::path& path() const noexcept { return path_; }
    /// Associates the buffer with @p path.
    void setPath(std::filesystem::path path) { path_ = std::move(path); }
    /// True when the text changed since the last save.
    bool dirty() const noexcept { return dirty_; }
    /// Clears the dirty flag; called after a successful save.
    void markClean() noexcept { dirty_ = false; }
    /// @}

    /// @name View state
    /// @{

    /// First visible line.
    std::size_t topLine() const noexcept { return topLine_; }
    /// Scrolls so that @p line is the first visible line.
    void setTopLine(std::size_t line) noexcept { topLine_ = line; }
    /// First visible display column.
    std::size_t leftColumn() const noexcept { return leftColumn_; }
    /// Scrolls horizontally to display column @p column.
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
