#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <proj/tui/minibuffer.h>
#include <proj/tui/utf8.h>

namespace tui {

namespace {

// The candidates that start with @p text, in order.
std::vector<std::string> prefixMatches(const std::vector<std::string>& candidates, std::string_view text) {
    std::vector<std::string> out;
    std::ranges::copy_if(candidates, std::back_inserter(out),
                         [&](const std::string& c) { return c.starts_with(text); });
    return out;
}

}  // namespace

std::vector<std::string> filterCandidates(const std::vector<std::string>& candidates,
                                          std::string_view text) {
    std::vector<std::string> out = prefixMatches(candidates, text);
    std::ranges::copy_if(candidates, std::back_inserter(out), [&](const std::string& c) {
        return !c.starts_with(text) && c.find(text) != std::string::npos;
    });
    return out;
}

std::string commonPrefix(const std::vector<std::string>& values) {
    if (values.empty()) return {};
    std::string prefix = values.front();
    for (const auto& v : values) {
        const auto mismatch = std::ranges::mismatch(prefix, v);
        prefix.erase(mismatch.in1, prefix.end());
    }
    // Do not end inside a multi-byte sequence.
    while (!prefix.empty() && !utf8::isValid(prefix)) prefix.pop_back();
    return prefix;
}

void minibuffer::open(std::string prompt, std::string initial, std::vector<std::string> candidates) {
    active_ = true;
    prompt_ = std::move(prompt);
    candidates_ = std::move(candidates);
    setText(std::move(initial));
}

void minibuffer::setText(std::string text) {
    text_ = std::move(text);
    cursor_ = utf8::length(text_);
}

void minibuffer::insert(std::string_view s) {
    const std::size_t at = utf8::byteOffset(text_, cursor_);
    text_.insert(at, s);
    cursor_ += utf8::length(s);
}

void minibuffer::deleteBackward() {
    if (cursor_ == 0) return;
    const std::size_t end = utf8::byteOffset(text_, cursor_);
    const std::size_t begin = utf8::byteOffset(text_, cursor_ - 1);
    text_.erase(begin, end - begin);
    --cursor_;
}

void minibuffer::deleteForward() {
    if (cursor_ >= utf8::length(text_)) return;
    const std::size_t begin = utf8::byteOffset(text_, cursor_);
    const std::size_t end = utf8::byteOffset(text_, cursor_ + 1);
    text_.erase(begin, end - begin);
}

void minibuffer::moveLeft() {
    if (cursor_ > 0) --cursor_;
}

void minibuffer::moveRight() {
    if (cursor_ < utf8::length(text_)) ++cursor_;
}

void minibuffer::moveEnd() { cursor_ = utf8::length(text_); }

bool minibuffer::complete() {
    const std::string prefix = commonPrefix(prefixMatches(candidates_, text_));
    if (prefix.size() <= text_.size()) return false;
    setText(prefix);
    return true;
}

} // namespace tui
