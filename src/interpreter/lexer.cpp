#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/interpreter/lexer.h>

namespace interpreter {

namespace {

bool isWhitespace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Characters that end a word (`grammar.md` §1): whitespace, brackets,
// quote, string start, and comment start.
bool isDelimiter(char c) noexcept {
    return isWhitespace(c) || c == '(' || c == ')' || c == '[' || c == ']' || c == '\'' ||
           c == '"' || c == ';';
}

bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

bool isLetter(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool isSymbolStart(char c) noexcept {
    switch (c) {
    case '+': case '-': case '*': case '/': case '<': case '>':
    case '=': case '!': case '?': case '_': case '%':
        return true;
    default:
        return isLetter(c);
    }
}

bool isSymbolChar(char c) noexcept { return isSymbolStart(c) || isDigit(c) || c == '.'; }

bool allSymbolChars(std::string_view text) noexcept { return std::ranges::all_of(text, isSymbolChar); }

// Length of the numeric prefix of `word` per the integer and float
// productions, and whether it is a float. Returns 0 when the prefix is
// not a well-formed number.
std::pair<std::size_t, bool> scanNumber(std::string_view word) noexcept {
    std::size_t i = 0;
    if (i < word.size() && word[i] == '-') ++i;
    const std::size_t intStart = i;
    while (i < word.size() && isDigit(word[i])) ++i;
    if (i == intStart) return {0, false};
    bool isFloat = false;
    if (i < word.size() && word[i] == '.') {
        const std::size_t fracStart = ++i;
        while (i < word.size() && isDigit(word[i])) ++i;
        if (i == fracStart) return {0, false};
        isFloat = true;
    }
    if (i < word.size() && (word[i] == 'e' || word[i] == 'E')) {
        std::size_t j = i + 1;
        if (j < word.size() && (word[j] == '+' || word[j] == '-')) ++j;
        const std::size_t expStart = j;
        while (j < word.size() && isDigit(word[j])) ++j;
        if (j > expStart) {
            i = j;
            isFloat = true;
        }
    }
    return {i, isFloat};
}

class lexer {
public:
    lexer(std::string_view source, bool strict) : src_(source), strict_(strict) {}

    lexResult run() {
        lexResult result;
        while (skipWhitespace()) {
            token t = next();
            if (t.kind == tokenKind::error && strict_) {
                result.failure = t;
                break;
            }
            result.tokens.push_back(t);
        }
        return result;
    }

private:
    bool skipWhitespace() noexcept {
        while (pos_ < src_.size() && isWhitespace(src_[pos_])) advance(1);
        return pos_ < src_.size();
    }

    void advance(std::size_t bytes) noexcept {
        for (std::size_t end = pos_ + bytes; pos_ < end && pos_ < src_.size(); ++pos_) {
            const auto c = static_cast<unsigned char>(src_[pos_]);
            if (c == '\n') {
                ++line_;
                column_ = 1;
            } else if ((c & 0xC0u) != 0x80u) {
                ++column_;  // count code points, not continuation bytes
            }
        }
    }

    char peek(std::size_t ahead = 0) const noexcept {
        return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
    }

    token begin(tokenKind kind) const noexcept {
        token t{.kind = kind};
        t.offset = static_cast<std::uint32_t>(pos_);
        t.line = line_;
        t.column = column_;
        return t;
    }

    token finish(token t) const noexcept {
        t.length = static_cast<std::uint32_t>(pos_ - t.offset);
        return t;
    }

    token fail(token t, lexError error) const noexcept {
        t.kind = tokenKind::error;
        t.error = error;
        return finish(t);
    }

    token next() {
        switch (peek()) {
        case '(': return single(tokenKind::leftParen);
        case ')': return single(tokenKind::rightParen);
        case '[': return single(tokenKind::leftBracket);
        case ']': return single(tokenKind::rightBracket);
        case '\'': return single(tokenKind::quote);
        case '"': return stringToken();
        case ';': return lineComment();
        case '#':
            if (peek(1) == '|') return blockComment();
            if (peek(1) == ';') {
                token t = begin(tokenKind::datumComment);
                advance(2);
                return finish(t);
            }
            return word();
        default: return word();
        }
    }

    token single(tokenKind kind) noexcept {
        token t = begin(kind);
        advance(1);
        return finish(t);
    }

    token lineComment() noexcept {
        token t = begin(tokenKind::lineComment);
        while (pos_ < src_.size() && src_[pos_] != '\n') advance(1);
        return finish(t);
    }

    token blockComment() noexcept {
        token t = begin(tokenKind::blockComment);
        advance(2);
        int depth = 1;
        while (pos_ < src_.size()) {
            if (peek() == '#' && peek(1) == '|') {
                ++depth;
                advance(2);
            } else if (peek() == '|' && peek(1) == '#') {
                advance(2);
                if (--depth == 0) return finish(t);
            } else {
                advance(1);
            }
        }
        return fail(t, lexError::unterminatedBlockComment);
    }

    token stringToken() noexcept {
        token t = begin(tokenKind::string);
        advance(1);
        lexError error = lexError::none;
        while (pos_ < src_.size()) {
            const char c = peek();
            if (c == '"') {
                advance(1);
                return error == lexError::none ? finish(t) : fail(t, error);
            }
            if (c == '\\') {
                const char e = peek(1);
                if (e == '\0' && pos_ + 1 >= src_.size()) break;
                if (e != 'n' && e != 't' && e != 'r' && e != '"' && e != '\\' &&
                    error == lexError::none) {
                    error = lexError::invalidEscape;
                }
                advance(2);
                continue;
            }
            advance(1);
        }
        return fail(t, lexError::unterminatedString);
    }

    token word() noexcept {
        token t = begin(tokenKind::symbol);
        std::size_t end = pos_;
        while (end < src_.size() && !isDelimiter(src_[end])) ++end;
        const std::string_view text = src_.substr(pos_, end - pos_);
        advance(text.size());

        const char first = text[0];
        if (isDigit(first) || (first == '-' && text.size() > 1 && isDigit(text[1]))) {
            return numberToken(t, text);
        }
        if (first == ':') {
            if (text.size() == 1) return fail(t, lexError::emptyKeyword);
            if (!allSymbolChars(text.substr(1))) return fail(t, lexError::unexpectedCharacter);
            t.kind = tokenKind::keyword;
            return finish(t);
        }
        if (first == '#') {
            if (text == "#t" || text == "#f") {
                t.kind = tokenKind::boolean;
                return finish(t);
            }
            return fail(t, lexError::invalidHashSyntax);
        }
        if (text == "nil") {
            t.kind = tokenKind::nil;
            return finish(t);
        }
        if (text == "&optional" || text == "&rest" || text == "&key") {
            return finish(t);  // lambda-list markers (`grammar.md` §2)
        }
        if (!isSymbolStart(first) || !allSymbolChars(text)) {
            return fail(t, lexError::unexpectedCharacter);
        }
        return finish(t);
    }

    token numberToken(token t, std::string_view text) const noexcept {
        const auto [numeric, isFloat] = scanNumber(text);
        if (numeric == 0) return fail(t, lexError::malformedNumber);
        if (numeric == text.size()) {
            t.kind = isFloat ? tokenKind::floating : tokenKind::integer;
            return finish(t);
        }
        if (!parseUnitSuffix(text.substr(numeric))) return fail(t, lexError::malformedNumber);
        t.kind = tokenKind::unitLiteral;
        t.numericLength = static_cast<std::uint32_t>(numeric);
        return finish(t);
    }

    std::string_view src_;
    bool strict_;
    std::size_t pos_ = 0;
    std::uint32_t line_ = 1;
    std::uint32_t column_ = 1;
};

} // namespace

std::string_view describe(lexError error) noexcept {
    switch (error) {
    case lexError::none: return "no error";
    case lexError::unterminatedString: return "unterminated string";
    case lexError::unterminatedBlockComment: return "unterminated block comment";
    case lexError::invalidEscape: return "invalid escape sequence in string";
    case lexError::malformedNumber: return "malformed number or unknown unit suffix";
    case lexError::emptyKeyword: return "keyword has no name";
    case lexError::invalidHashSyntax: return "unknown # syntax";
    case lexError::unexpectedCharacter: return "unexpected character";
    }
    return "unknown error";
}

namespace {

constexpr std::array<std::pair<std::string_view, unitSuffix>, 12> suffixes{{
    {"s", unitSuffix::s},
    {"ms", unitSuffix::ms},
    {"us", unitSuffix::us},
    {"smp", unitSuffix::smp},
    {"beat", unitSuffix::beat},
    {"bar", unitSuffix::bar},
    {"hz", unitSuffix::hz},
    {"khz", unitSuffix::khz},
    {"bpm", unitSuffix::bpm},
    {"db", unitSuffix::db},
    {"st", unitSuffix::st},
    {"ct", unitSuffix::ct},
}};

}  // namespace

std::optional<unitSuffix> parseUnitSuffix(std::string_view text) noexcept {
    for (const auto& [name, suffix] : suffixes) {
        if (name == text) return suffix;
    }
    return std::nullopt;
}

std::string unitSuffixNames() {
    std::string out;
    for (const auto& entry : suffixes) {
        if (!out.empty()) out += ' ';
        out += entry.first;
    }
    return out;
}

lexResult lexStrict(std::string_view source) { return lexer(source, true).run(); }

std::vector<token> lexTolerant(std::string_view source) {
    return lexer(source, false).run().tokens;
}

} // namespace interpreter
