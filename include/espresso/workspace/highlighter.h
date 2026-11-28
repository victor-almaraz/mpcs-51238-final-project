/// @file
/// Maps lexer tokens to highlight categories (`tui.md` §6). Has no
/// terminal dependency; styles are applied by the view.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/interpreter/lexer.h>

namespace workspace {

/// Highlight category of a span of text (`tui.md` §6.2).
enum class highlightCategory : std::uint8_t {
    comment,      ///< Line, block, and datum comments, and the datum a `#;` comments out
    string,       ///< String literals
    number,       ///< Integer and floating-point literals
    unitLiteral,  ///< Numbers with a unit, such as `250ms`
    keyword,      ///< Keywords, booleans, and nil
    specialForm,  ///< Special forms and definition forms
    objectForm,   ///< Object system forms, such as `defclass`
    symbol,       ///< Other symbols
    delimiter,    ///< Brackets and quotes
    error,        ///< Text the lexer rejects
};

/// Number of highlightCategory values.
inline constexpr std::size_t highlightCategoryCount = static_cast<std::size_t>(highlightCategory::error) + 1;

/// A highlighted byte range.
struct highlightSpan {
    /// Byte offset of the first byte.
    std::size_t offset = 0;
    /// Length in bytes.
    std::size_t length = 0;
    /// How the span is drawn.
    highlightCategory category = highlightCategory::symbol;
};

/// Result of highlighting a text: spans in source order, and the tokens
/// they were built from.
struct highlightResult {
    /// Spans in source order, one per token.
    std::vector<highlightSpan> spans;
    /// Tokens from the tolerant lexer.
    std::vector<interpreter::token> tokens;
};

/// Highlights @p source with the tolerant lexer. The datum after a `#;`
/// datum comment is highlighted as a comment.
highlightResult highlight(std::string_view source);

/// Category of a symbol by name: special form, object form, or symbol.
highlightCategory classifySymbol(std::string_view name) noexcept;

/// Byte offset of the delimiter matching the one at @p offset, using
/// @p tokens from highlight(). Returns nothing when @p offset is not on
/// a bracket or the bracket is unmatched (`tui.md` §6.5).
std::optional<std::size_t> matchingDelimiter(const std::vector<interpreter::token>& tokens,
                                             std::size_t offset);

/// Byte range [first, second) of the datum that ends at or before
/// @p offset, including leading quotes. This is the form `evalLastForm`
/// evaluates. Returns nothing when no complete datum precedes @p offset.
std::optional<std::pair<std::size_t, std::size_t>> formBefore(const std::vector<interpreter::token>& tokens,
                                                              std::size_t offset);

}  // namespace workspace
