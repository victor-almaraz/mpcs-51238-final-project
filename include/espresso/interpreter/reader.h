/// @file
/// Reader: turns source text into syntax trees with source spans
/// (`interpreter.md` §3).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/interpreter/lexer.h>

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
    nil,       ///< `nil`
    boolean,   ///< `#t` or `#f`, in datum::boolean
    integer,   ///< An integer, in datum::integer
    floating,  ///< A float, in datum::floating
    quantity,  ///< A unit literal, in datum::quantity
    string,    ///< A string, unescaped in datum::text
    keyword,   ///< A keyword, its name without the colon in datum::text
    symbol,    ///< A symbol, its name in datum::text
    list,      ///< `( ... )`, its elements in datum::items
    vector,    ///< `[ ... ]`, its elements in datum::items
};

/// A unit literal as written: its numeric value and suffix. Conversion
/// to canonical units belongs to the units layer (`interpreter.md` §8).
struct quantityLiteral {
    /// Numeric part, as written.
    double value = 0.0;
    /// Unit suffix.
    unitSuffix unit = unitSuffix::s;
};

/// A syntax-tree node. `'x` reads as the list `(quote x)`.
struct datum {
    /// Kind of node.
    datumKind kind = datumKind::nil;
    /// Location in the source.
    sourceSpan span;
    /// Value of a boolean.
    bool boolean = false;
    /// Value of an integer.
    std::int64_t integer = 0;
    /// Value of a float.
    double floating = 0.0;
    /// Value of a unit literal.
    quantityLiteral quantity;
    std::string text;          ///< String contents (unescaped), or keyword/symbol name
    std::vector<datum> items;  ///< List or vector elements

    /// True when this datum is a symbol named @p name.
    bool isSymbol(std::string_view name) const noexcept { return kind == datumKind::symbol && text == name; }
};

/// A read error with the span of the offending text.
struct readError {
    /// Description of the error.
    std::string message;
    /// Location of the offending text.
    sourceSpan span;
};

/// Result of reading a source text.
struct readResult {
    std::vector<datum> data;         ///< Top-level data, in order
    std::optional<readError> error;  ///< Set when reading failed
};

/// Reads every datum in @p source using the strict lexer.
readResult read(std::string_view source);

}  // namespace interpreter
