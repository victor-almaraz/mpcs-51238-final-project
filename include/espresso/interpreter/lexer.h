/// @file
/// Lexer for the Lisp dialect (`grammar.md` §1, `interpreter.md` §3).

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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
    none,                      ///< Not malformed
    unterminatedString,        ///< `"` with no closing `"`
    unterminatedBlockComment,  ///< `#|` with no matching `|#`
    invalidEscape,             ///< `\` in a string not followed by `n`, `t`, `r`, `"`, or `\`
    malformedNumber,           ///< Numeric token with a bad suffix, such as `2x` (`grammar.md` §1.4)
    emptyKeyword,              ///< `:` with no name
    invalidHashSyntax,         ///< `#` not followed by `t`, `f`, `|`, or `;`
    unexpectedCharacter,       ///< A word holding a character no symbol or keyword may hold there
};

/// Returns a human-readable description of a lexical error.
std::string_view describe(lexError error) noexcept;

/// Unit suffix of a unit literal (`grammar.md` §5).
enum class unitSuffix : std::uint8_t {
    s,     ///< Seconds
    ms,    ///< Milliseconds
    us,    ///< Microseconds
    smp,   ///< Samples, at the compilation's sample rate
    beat,  ///< Beats
    bar,   ///< Bars of 4 beats
    hz,    ///< Hertz
    khz,   ///< Kilohertz
    bpm,   ///< Beats per minute, a frequency
    db,    ///< Decibels, a level
    st,    ///< Semitones
    ct,    ///< Cents, hundredths of a semitone
};

/// Returns the suffix spelled by @p text, if any.
std::optional<unitSuffix> parseUnitSuffix(std::string_view text) noexcept;

/// Every unit suffix, as written, separated by spaces: for messages.
std::string unitSuffixNames();

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
    std::string_view text(std::string_view source) const noexcept { return source.substr(offset, length); }
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

}  // namespace interpreter
