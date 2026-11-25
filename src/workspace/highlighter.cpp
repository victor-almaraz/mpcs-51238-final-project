#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/interpreter/lexer.h>
#include <espresso/workspace/highlighter.h>

namespace workspace {

using interpreter::token;
using interpreter::tokenKind;

namespace {

bool isOpen(tokenKind k) noexcept { return k == tokenKind::leftParen || k == tokenKind::leftBracket; }

bool isClose(tokenKind k) noexcept { return k == tokenKind::rightParen || k == tokenKind::rightBracket; }

bool isTrivia(tokenKind k) noexcept { return k == tokenKind::lineComment || k == tokenKind::blockComment; }

highlightCategory categoryOf(const token& t, std::string_view source) noexcept {
    switch (t.kind) {
    case tokenKind::leftParen:
    case tokenKind::rightParen:
    case tokenKind::leftBracket:
    case tokenKind::rightBracket:
    case tokenKind::quote: return highlightCategory::delimiter;
    case tokenKind::integer:
    case tokenKind::floating: return highlightCategory::number;
    case tokenKind::unitLiteral: return highlightCategory::unitLiteral;
    case tokenKind::string: return highlightCategory::string;
    case tokenKind::boolean:
    case tokenKind::nil:
    case tokenKind::keyword: return highlightCategory::keyword;
    case tokenKind::symbol: return classifySymbol(t.text(source));
    case tokenKind::lineComment:
    case tokenKind::blockComment:
    case tokenKind::datumComment: return highlightCategory::comment;
    case tokenKind::error: return highlightCategory::error;
    }
    return highlightCategory::symbol;
}

// Index just past the datum starting at token `i`, skipping comments.
std::size_t skipDatum(const std::vector<token>& tokens, std::size_t i) {
    while (i < tokens.size() && isTrivia(tokens[i].kind)) ++i;
    if (i >= tokens.size()) return i;
    const tokenKind k = tokens[i].kind;
    if (k == tokenKind::quote || k == tokenKind::datumComment) return skipDatum(tokens, i + 1);
    if (!isOpen(k)) return i + 1;
    int depth = 0;
    for (; i < tokens.size(); ++i) {
        if (isOpen(tokens[i].kind)) ++depth;
        if (isClose(tokens[i].kind) && --depth == 0) return i + 1;
    }
    return i;
}

}  // namespace

highlightCategory classifySymbol(std::string_view name) noexcept {
    // Special forms (`interpreter.md` §5.1) and the definition forms of
    // `grammar.md` §4.
    static constexpr std::array<std::string_view, 15> special{
        "quote",  "if",     "cond",   "let",      "fn",      "do",     "set!", "define",
        "module", "import", "export", "defpiece", "project", "mapper", "play",
    };
    static constexpr std::array<std::string_view, 7> object{
        "defclass", "defgeneric", "defmethod", "make", "slot-value", "call-next-method", "next-method-p",
    };
    if (std::ranges::find(special, name) != special.end()) {
        return highlightCategory::specialForm;
    }
    if (std::ranges::find(object, name) != object.end()) {
        return highlightCategory::objectForm;
    }
    return highlightCategory::symbol;
}

highlightResult highlight(std::string_view source) {
    highlightResult result;
    result.tokens = interpreter::lexTolerant(source);
    const auto& tokens = result.tokens;
    result.spans.reserve(tokens.size());
    std::size_t commentedUntil = 0;  // token index; datum-commented tokens before it
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const token& t = tokens[i];
        if (t.kind == tokenKind::datumComment && i >= commentedUntil) {
            commentedUntil = skipDatum(tokens, i + 1);
        }
        const highlightCategory category = i < commentedUntil && t.kind != tokenKind::error
                                               ? highlightCategory::comment
                                               : categoryOf(t, source);
        result.spans.push_back({.offset = t.offset, .length = t.length, .category = category});
    }
    return result;
}

std::optional<std::size_t> matchingDelimiter(const std::vector<token>& tokens, std::size_t offset) {
    const auto it = std::ranges::lower_bound(tokens, offset, {}, &token::offset);
    if (it == tokens.end() || it->offset != offset) return std::nullopt;
    const auto index = static_cast<std::size_t>(it - tokens.begin());
    const tokenKind kind = it->kind;
    if (isOpen(kind)) {
        int depth = 0;
        for (std::size_t i = index; i < tokens.size(); ++i) {
            if (isOpen(tokens[i].kind)) ++depth;
            if (isClose(tokens[i].kind) && --depth == 0) return tokens[i].offset;
        }
    } else if (isClose(kind)) {
        int depth = 0;
        for (std::size_t i = index + 1; i-- > 0;) {
            if (isClose(tokens[i].kind)) ++depth;
            if (isOpen(tokens[i].kind) && --depth == 0) return tokens[i].offset;
        }
    }
    return std::nullopt;
}

std::optional<std::pair<std::size_t, std::size_t>> formBefore(const std::vector<token>& tokens,
                                                              std::size_t offset) {
    std::size_t i = tokens.size();
    while (i > 0) {
        const token& t = tokens[i - 1];
        if (t.offset + t.length <= offset && !isTrivia(t.kind) && t.kind != tokenKind::datumComment) {
            break;
        }
        --i;
    }
    if (i == 0) return std::nullopt;
    const token& last = tokens[i - 1];
    if (isOpen(last.kind) || last.kind == tokenKind::quote || last.kind == tokenKind::error) {
        return std::nullopt;
    }
    std::size_t first = last.offset;
    std::size_t index = i - 1;
    if (isClose(last.kind)) {
        const auto open = matchingDelimiter(tokens, last.offset);
        if (!open) return std::nullopt;
        first = *open;
        while (tokens[index].offset != first) --index;
    }
    while (index > 0 && tokens[index - 1].kind == tokenKind::quote) first = tokens[--index].offset;
    return std::make_pair(first, static_cast<std::size_t>(last.offset + last.length));
}

}  // namespace workspace
