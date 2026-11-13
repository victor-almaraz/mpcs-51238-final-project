/// @file
/// Lexer for the Lisp dialect (`grammar.md` §1, `interpreter.md` §3).

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace interpreter {

/// Kind of a lexical token.
enum class tokenKind : std::uint8_t {
    leftParen,     ///< `(`
    rightParen,    ///< `)`
    leftBracket,   ///< `[`
    rightBracket,  ///< `]`
    quote,         ///< `'`
    integer,       ///< `440`, `-3`
    floating,      ///< `0.7`, `1e3`
    unitLiteral,   ///< `250ms`, `-6db`
    string,        ///< `"piano.sfz"`
    boolean,       ///< `#t`, `#f`
    nil,           ///< `nil`
    keyword,       ///< `:freq`
    symbol,        ///< `sine-osc`
    lineComment,   ///< `; note`
    blockComment,  ///< `#| ... |#`, nesting
    datumComment,  ///< `#;`, which comments out the next datum
    error,         ///< Malformed input; see token::error
};

/// Reason a token is malformed.
enum class lexError : std::uint8_t {
    none,
    unterminatedString,
    unterminatedBlockComment,
    invalidEscape,
    malformedNumber,     ///< Numeric token with a bad suffix, such as `2x` (`grammar.md` §1.4)
    emptyKeyword,        ///< `:` with no name
    invalidHashSyntax,   ///< `#` not followed by `t`, `f`, `|`, or `;`
    unexpectedCharacter,
};

/// Returns a human-readable description of a lexical error.
std::string_view describe(lexError error) noexcept;

/// Unit suffix of a unit literal (`grammar.md` §5).
enum class unitSuffix : std::uint8_t {
    s, ms, us, smp, beat, bar, hz, khz, bpm, db, st, ct,
};

/// Returns the suffix spelled by @p text, if any.
std::optional<unitSuffix> parseUnitSuffix(std::string_view text) noexcept;

/// A token. Text is not copied: offset and length index the source.
struct token {
    /// Kind of token.
    tokenKind kind;
    /// Why the token is malformed, for tokenKind::error; lexError::none otherwise.
    lexError error = lexError::none;
    std::uint32_t offset = 0;         ///< Byte offset of the first byte
    std::uint32_t length = 0;         ///< Length in bytes
    std::uint32_t line = 1;           ///< 1-based line
    std::uint32_t column = 1;         ///< 1-based column, in code points
    std::uint32_t numericLength = 0;  ///< For unit literals: bytes of the numeric part

    /// Returns the token's text within @p source.
    std::string_view text(std::string_view source) const noexcept {
        return source.substr(offset, length);
    }
};

/// Result of strict lexing.
struct lexResult {
    std::vector<token> tokens;     ///< Tokens before the first error, or all tokens
    std::optional<token> failure;  ///< The first malformed token, if any
};

/// Lexes @p source in strict mode, for compilation. Stops at the first
/// malformed token and reports it in lexResult::failure.
lexResult lexStrict(std::string_view source);

/// Lexes @p source in tolerant mode, for editor highlighting. Never
/// fails: malformed input becomes tokens of kind tokenKind::error, and
/// lexing continues after them. Whitespace produces no tokens.
std::vector<token> lexTolerant(std::string_view source);

} // namespace interpreter
