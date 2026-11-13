/// @file
/// Reader: turns source text into syntax trees with source spans
/// (`interpreter.md` §3).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <proj/interpreter/lexer.h>

namespace interpreter {

/// Location of a datum in its source file (`interpreter.md` §3.2).
/// The file is tracked by the caller.
struct sourceSpan {
    std::uint32_t offset = 0;  ///< Byte offset
    std::uint32_t length = 0;  ///< Length in bytes
    std::uint32_t line = 1;    ///< 1-based line
    std::uint32_t column = 1;  ///< 1-based column, in code points
};

/// Kind of a syntax-tree node.
enum class datumKind : std::uint8_t {
    nil, boolean, integer, floating, quantity, string, keyword, symbol, list, vector,
};

/// A unit literal as written: its numeric value and suffix. Conversion
/// to canonical units belongs to the units layer (`interpreter.md` §8).
struct quantityLiteral {
    double value = 0.0;
    unitSuffix unit = unitSuffix::s;
};

/// A syntax-tree node. `'x` reads as the list `(quote x)`.
struct datum {
    datumKind kind = datumKind::nil;
    sourceSpan span;
    bool boolean = false;
    std::int64_t integer = 0;
    double floating = 0.0;
    quantityLiteral quantity;
    std::string text;          ///< String contents (unescaped), or keyword/symbol name
    std::vector<datum> items;  ///< List or vector elements

    /// True when this datum is a symbol named @p name.
    bool isSymbol(std::string_view name) const noexcept {
        return kind == datumKind::symbol && text == name;
    }
};

/// A read error with the span of the offending text.
struct readError {
    std::string message;
    sourceSpan span;
};

/// Result of reading a source text.
struct readResult {
    std::vector<datum> data;         ///< Top-level data, in order
    std::optional<readError> error;  ///< Set when reading failed
};

/// Reads every datum in @p source using the strict lexer.
readResult read(std::string_view source);

} // namespace interpreter
