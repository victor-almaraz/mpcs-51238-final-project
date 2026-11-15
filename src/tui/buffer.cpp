#include <proj/tui/buffer.h>

#include <algorithm>

#include <proj/tui/utf8.h>

namespace tui {

// ---------------------------------------------------------------------
// gapBuffer

gapBuffer::gapBuffer(std::string_view text) : data_(text.begin(), text.end()) {
    gapStart_ = gapEnd_ = data_.size();
}

char gapBuffer::at(std::size_t offset) const noexcept {
    return offset < gapStart_ ? data_[offset] : data_[offset + gapLength()];
}

std::string gapBuffer::substr(std::size_t offset, std::size_t length) const {
    const std::size_t n = size();
    offset = std::min(offset, n);
    length = std::min(length, n - offset);
    std::string out;
    out.reserve(length);
    const std::size_t end = offset + length;
    if (offset < gapStart_) {
        const std::size_t first = std::min(end, gapStart_);
        out.append(it(offset), it(first));
        offset = first;
    }
    if (offset < end) out.append(it(offset + gapLength()), it(end + gapLength()));
    return out;
}

void gapBuffer::moveGap(std::size_t offset) {
    if (offset < gapStart_) {
        const std::size_t count = gapStart_ - offset;
        std::copy_backward(it(offset), it(gapStart_), it(gapEnd_));
        gapStart_ -= count;
        gapEnd_ -= count;
    } else if (offset > gapStart_) {
        const std::size_t count = offset - gapStart_;
        std::copy(it(gapEnd_), it(gapEnd_ + count), it(gapStart_));
        gapStart_ += count;
        gapEnd_ += count;
    }
}

void gapBuffer::reserveGap(std::size_t needed) {
    if (gapLength() >= needed) return;
    const std::size_t tail = data_.size() - gapEnd_;
    const std::size_t grow = std::max(needed - gapLength(), data_.size() / 2 + 64);
    data_.resize(data_.size() + grow);
    std::copy_backward(it(gapEnd_), it(gapEnd_ + tail), data_.end());
    gapEnd_ = data_.size() - tail;
}

void gapBuffer::insert(std::size_t offset, std::string_view text) {
    if (text.empty()) return;
    offset = std::min(offset, size());
    moveGap(offset);
    reserveGap(text.size());
    std::copy(text.begin(), text.end(), it(gapStart_));
    gapStart_ += text.size();
}

void gapBuffer::erase(std::size_t offset, std::size_t length) {
    offset = std::min(offset, size());
    length = std::min(length, size() - offset);
    moveGap(offset);
    gapEnd_ += length;
}

// ---------------------------------------------------------------------
// buffer: content

buffer::buffer(std::string_view text, std::filesystem::path path, undoLimits limits)
    : text_(text), path_(std::move(path)), limits_(limits) {}

const std::vector<std::size_t>& buffer::lineStarts() const {
    if (!linesValid_) {
        lineStarts_.assign(1, 0);
        const std::string all = text_.str();
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (all[i] == '\n') lineStarts_.push_back(i + 1);
        }
        linesValid_ = true;
    }
    return lineStarts_;
}

std::size_t buffer::lineCount() const { return lineStarts().size(); }

std::string buffer::line(std::size_t index) const {
    const auto& starts = lineStarts();
    if (index >= starts.size()) return {};
    const std::size_t begin = starts[index];
    const std::size_t end = index + 1 < starts.size() ? starts[index + 1] - 1 : text_.size();
    return text_.substr(begin, end - begin);
}

std::size_t buffer::lineLength(std::size_t index) const { return utf8::length(line(index)); }

position buffer::clamp(position pos) const {
    pos.line = std::min(pos.line, lineCount() - 1);
    pos.column = std::min(pos.column, lineLength(pos.line));
    return pos;
}

std::size_t buffer::offsetOf(position pos) const {
    pos = clamp(pos);
    return lineStarts()[pos.line] + utf8::byteOffset(line(pos.line), pos.column);
}

position buffer::positionOf(std::size_t offset) const {
    offset = std::min(offset, text_.size());
    const auto& starts = lineStarts();
    const auto it = std::upper_bound(starts.begin(), starts.end(), offset);
    const auto lineIndex = static_cast<std::size_t>(it - starts.begin()) - 1;
    const std::size_t begin = starts[lineIndex];
    return {lineIndex, utf8::length(text_.substr(begin, offset - begin))};
}

// ---------------------------------------------------------------------
// buffer: cursor and selection

void buffer::setCursor(position pos) {
    endMovement();
    cursor_ = clamp(pos);
}

std::pair<std::size_t, std::size_t> buffer::selectionRange() const {
    if (!mark_) return {cursorOffset(), cursorOffset()};
    const std::size_t a = offsetOf(*mark_);
    const std::size_t b = cursorOffset();
    return {std::min(a, b), std::max(a, b)};
}

std::string buffer::selectedText() const {
    const auto [first, last] = selectionRange();
    return text_.substr(first, last - first);
}

// ---------------------------------------------------------------------
// buffer: movement

void buffer::endMovement() noexcept {
    mergeable_ = false;
    goalColumn_.reset();
}

void buffer::moveLeft() {
    endMovement();
    if (cursor_.column > 0) {
        --cursor_.column;
    } else if (cursor_.line > 0) {
        --cursor_.line;
        cursor_.column = lineLength(cursor_.line);
    }
}

void buffer::moveRight() {
    endMovement();
    if (cursor_.column < lineLength(cursor_.line)) {
        ++cursor_.column;
    } else if (cursor_.line + 1 < lineCount()) {
        ++cursor_.line;
        cursor_.column = 0;
    }
}

void buffer::moveUp(std::size_t lines) {
    mergeable_ = false;
    const std::size_t goal = goalColumn_.value_or(cursor_.column);
    cursor_.line -= std::min(lines, cursor_.line);
    cursor_.column = std::min(goal, lineLength(cursor_.line));
    goalColumn_ = goal;
}

void buffer::moveDown(std::size_t lines) {
    mergeable_ = false;
    const std::size_t goal = goalColumn_.value_or(cursor_.column);
    cursor_.line = std::min(cursor_.line + lines, lineCount() - 1);
    cursor_.column = std::min(goal, lineLength(cursor_.line));
    goalColumn_ = goal;
}

void buffer::moveLineStart() {
    endMovement();
    cursor_.column = 0;
}

void buffer::moveLineEnd() {
    endMovement();
    cursor_.column = lineLength(cursor_.line);
}

bool buffer::isWordByte(std::size_t offset) const noexcept {
    const auto c = static_cast<unsigned char>(text_.at(offset));
    return c >= 0x80u || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

void buffer::moveWordForward() {
    std::size_t off = cursorOffset();
    const std::size_t n = text_.size();
    while (off < n && !isWordByte(off)) ++off;
    while (off < n && isWordByte(off)) ++off;
    setCursor(positionOf(off));
}

void buffer::moveWordBackward() {
    std::size_t off = cursorOffset();
    while (off > 0 && !isWordByte(off - 1)) --off;
    while (off > 0 && isWordByte(off - 1)) --off;
    setCursor(positionOf(off));
}

void buffer::moveBufferStart() { setCursor({0, 0}); }

void buffer::moveBufferEnd() {
    const std::size_t last = lineCount() - 1;
    setCursor({last, lineLength(last)});
}

void buffer::scrollToCursor(std::size_t rows) {
    rows = std::max<std::size_t>(rows, 1);
    if (cursor_.line < topLine_) topLine_ = cursor_.line;
    if (cursor_.line >= topLine_ + rows) topLine_ = cursor_.line - rows + 1;
}

// ---------------------------------------------------------------------
// buffer: edits

void buffer::apply(std::size_t offset, std::size_t length, std::string_view text) {
    text_.erase(offset, length);
    text_.insert(offset, text);
    linesValid_ = false;
    ++version_;
    dirty_ = true;
    mark_.reset();
    goalColumn_.reset();
}

void buffer::replace(std::size_t offset, std::size_t length, std::string_view text) {
    offset = std::min(offset, text_.size());
    length = std::min(length, text_.size() - offset);
    if (length == 0 && text.empty()) return;
    edit e{offset, text_.substr(offset, length), std::string(text), cursor_, {}};
    apply(offset, length, text);
    cursor_ = positionOf(offset + text.size());
    e.after = cursor_;
    record(std::move(e), false);
}

void buffer::insert(std::string_view text) {
    if (hasSelection()) {
        const auto [first, last] = selectionRange();
        replace(first, last - first, text);
        return;
    }
    const bool single = !text.empty() && text != "\n" &&
                        utf8::sequenceLength(static_cast<unsigned char>(text[0])) == text.size();
    const std::size_t offset = cursorOffset();
    edit e{offset, {}, std::string(text), cursor_, {}};
    apply(offset, 0, text);
    cursor_ = positionOf(offset + text.size());
    e.after = cursor_;
    record(std::move(e), single);
}

bool buffer::deleteBackward() {
    if (hasSelection()) return !deleteSelection().empty();
    const std::size_t off = cursorOffset();
    if (off == 0) return false;
    std::size_t prev = off - 1;
    while (prev > 0 && utf8::isContinuation(static_cast<unsigned char>(text_.at(prev)))) --prev;
    replace(prev, off - prev, {});
    return true;
}

bool buffer::deleteForward() {
    if (hasSelection()) return !deleteSelection().empty();
    const std::size_t off = cursorOffset();
    if (off >= text_.size()) return false;
    replace(off, utf8::sequenceLength(static_cast<unsigned char>(text_.at(off))), {});
    return true;
}

bool buffer::joinLines() {
    if (cursor_.line + 1 >= lineCount()) return false;
    const std::size_t end = lineStarts()[cursor_.line + 1] - 1;
    cursor_ = positionOf(end);
    replace(end, 1, {});
    return true;
}

std::string buffer::deleteSelection() {
    if (!hasSelection()) return {};
    const auto [first, last] = selectionRange();
    std::string removed = text_.substr(first, last - first);
    replace(first, last - first, {});
    return removed;
}

std::string buffer::killLine() {
    const std::size_t off = cursorOffset();
    const std::size_t len = lineLength(cursor_.line);
    std::size_t end = 0;
    if (cursor_.column < len) {
        end = offsetOf({cursor_.line, len});
    } else if (cursor_.line + 1 < lineCount()) {
        end = off + 1;
    } else {
        return {};
    }
    std::string removed = text_.substr(off, end - off);
    replace(off, end - off, {});
    return removed;
}

// ---------------------------------------------------------------------
// buffer: undo

void buffer::record(edit e, bool mergeableInsert) {
    redo_.clear();
    constexpr std::size_t maxGroup = 20;  // Emacs groups up to 20 typed characters
    if (mergeableInsert && mergeable_ && !undo_.empty()) {
        edit& last = undo_.back();
        if (last.removed.empty() && last.offset + last.inserted.size() == e.offset &&
            utf8::length(last.inserted) < maxGroup) {
            last.inserted += e.inserted;
            last.after = e.after;
            undoBytes_ += e.inserted.size();
            trimUndo();
            return;
        }
    }
    undoBytes_ += e.bytes();
    undo_.push_back(std::move(e));
    mergeable_ = mergeableInsert;
    trimUndo();
}

// Keeps the newest change unconditionally, then older changes while the
// total stays within the hard limit, stopping after the first change
// that crosses the soft limit (Emacs `truncate_undo_list`).
void buffer::trimUndo() {
    if (undo_.size() <= 1 || undoBytes_ <= limits_.soft) return;
    std::size_t kept = 0;
    std::size_t total = 0;
    for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) {
        const std::size_t size = it->bytes();
        if (kept > 0 && total + size > limits_.hard) break;
        total += size;
        ++kept;
        if (total > limits_.soft) break;
    }
    while (undo_.size() > kept) undo_.pop_front();
    undoBytes_ = total;
}

bool buffer::undo() {
    if (undo_.empty()) return false;
    edit e = std::move(undo_.back());
    undo_.pop_back();
    undoBytes_ -= e.bytes();
    apply(e.offset, e.inserted.size(), e.removed);
    cursor_ = clamp(e.before);
    mergeable_ = false;
    redo_.push_back(std::move(e));
    return true;
}

bool buffer::redo() {
    if (redo_.empty()) return false;
    edit e = std::move(redo_.back());
    redo_.pop_back();
    apply(e.offset, e.removed.size(), e.inserted);
    cursor_ = clamp(e.after);
    mergeable_ = false;
    undoBytes_ += e.bytes();
    undo_.push_back(std::move(e));
    trimUndo();
    return true;
}

} // namespace tui
