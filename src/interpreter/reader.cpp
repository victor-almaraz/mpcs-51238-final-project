#include <charconv>
#include <cstdlib>
#include <memory>
#include <string>

#include <proj/interpreter/reader.h>

namespace interpreter {

namespace {

sourceSpan spanOf(const token& t) noexcept { return {t.offset, t.length, t.line, t.column}; }

sourceSpan spanBetween(const sourceSpan& first, const token& last) noexcept {
    sourceSpan s = first;
    s.length = last.offset + last.length - first.offset;
    return s;
}

std::string unescape(std::string_view quoted) {
    std::string out;
    out.reserve(quoted.size());
    for (std::size_t i = 1; i + 1 < quoted.size(); ++i) {
        char c = quoted[i];
        if (c == '\\' && i + 2 < quoted.size()) {
            switch (quoted[++i]) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            default: c = quoted[i]; break;  // '"' and '\\'
            }
        }
        out.push_back(c);
    }
    return out;
}

double parseDouble(std::string_view text) {
    // std::from_chars for double is unavailable in some standard libraries.
    const std::string copy(text);
    return std::strtod(copy.c_str(), nullptr);
}

class reader {
public:
    reader(std::string_view source, std::vector<token> tokens)
        : src_(source), tokens_(std::move(tokens)) {}

    readResult run() {
        readResult result;
        while (!error_ && skipComments()) {
            datum d;
            if (!readDatum(d)) break;
            result.data.push_back(std::move(d));
        }
        result.error = std::move(error_);
        return result;
    }

    void fail(std::string message, sourceSpan span) {
        if (!error_) error_ = readError{std::move(message), span};
    }

private:
    // Skips comment tokens, including `#;` datum comments. Returns
    // false at end of input.
    bool skipComments() {
        while (pos_ < tokens_.size()) {
            const tokenKind k = tokens_[pos_].kind;
            if (k == tokenKind::lineComment || k == tokenKind::blockComment) {
                ++pos_;
            } else if (k == tokenKind::datumComment) {
                const token marker = tokens_[pos_++];
                if (!skipComments()) {
                    fail("#; is not followed by a datum", spanOf(marker));
                    return false;
                }
                datum ignored;
                if (!readDatum(ignored)) return false;
            } else {
                return true;
            }
        }
        return false;
    }

    // Nesting is bounded so deeply nested input fails with an error
    // instead of exhausting the stack.
    static constexpr int maxDepth = 512;

    bool readDatum(datum& out) {
        if (depth_ >= maxDepth) {
            fail("forms are nested too deeply", spanOf(tokens_[pos_]));
            return false;
        }
        ++depth_;
        const bool ok = readOne(out);
        --depth_;
        return ok;
    }

    bool readOne(datum& out) {
        const token t = tokens_[pos_++];
        out.span = spanOf(t);
        const std::string_view text = t.text(src_);
        switch (t.kind) {
        case tokenKind::leftParen:
            return readSequence(out, datumKind::list, tokenKind::rightParen, t);
        case tokenKind::leftBracket:
            return readSequence(out, datumKind::vector, tokenKind::rightBracket, t);
        case tokenKind::rightParen:
        case tokenKind::rightBracket:
            fail("unexpected closing delimiter", out.span);
            return false;
        case tokenKind::quote: {
            if (!skipComments()) {
                fail("' is not followed by a datum", out.span);
                return false;
            }
            datum quoted;
            if (!readDatum(quoted)) return false;
            out.kind = datumKind::list;
            datum head;
            head.kind = datumKind::symbol;
            head.text = "quote";
            head.span = spanOf(t);
            out.span.length = quoted.span.offset + quoted.span.length - out.span.offset;
            out.items.push_back(std::move(head));
            out.items.push_back(std::move(quoted));
            return true;
        }
        case tokenKind::integer: {
            out.kind = datumKind::integer;
            const auto r =
                std::from_chars(std::to_address(text.begin()), std::to_address(text.end()), out.integer);
            if (r.ec != std::errc{}) {
                fail("integer out of range", out.span);
                return false;
            }
            return true;
        }
        case tokenKind::floating:
            out.kind = datumKind::floating;
            out.floating = parseDouble(text);
            return true;
        case tokenKind::unitLiteral: {
            out.kind = datumKind::quantity;
            out.quantity.value = parseDouble(text.substr(0, t.numericLength));
            // The lexer classifies a token as a unit literal only when its
            // suffix parses; a miss here is a lexer bug, reported as such.
            const auto unit = parseUnitSuffix(text.substr(t.numericLength));
            if (!unit) {
                fail("malformed unit literal", out.span);
                return false;
            }
            out.quantity.unit = *unit;
            return true;
        }
        case tokenKind::string:
            out.kind = datumKind::string;
            out.text = unescape(text);
            return true;
        case tokenKind::boolean:
            out.kind = datumKind::boolean;
            out.boolean = text == "#t";
            return true;
        case tokenKind::nil:
            out.kind = datumKind::nil;
            return true;
        case tokenKind::keyword:
            out.kind = datumKind::keyword;
            out.text = std::string(text.substr(1));
            return true;
        case tokenKind::symbol:
            out.kind = datumKind::symbol;
            out.text = std::string(text);
            return true;
        case tokenKind::lineComment:
        case tokenKind::blockComment:
        case tokenKind::datumComment:
        case tokenKind::error:
            break;
        }
        fail("unexpected token", out.span);
        return false;
    }

    bool readSequence(datum& out, datumKind kind, tokenKind close, const token& open) {
        out.kind = kind;
        while (skipComments()) {
            const token& t = tokens_[pos_];
            if (t.kind == close) {
                ++pos_;
                out.span = spanBetween(out.span, t);
                return true;
            }
            if (t.kind == tokenKind::rightParen || t.kind == tokenKind::rightBracket) {
                fail("mismatched closing delimiter", spanOf(t));
                return false;
            }
            datum item;
            if (!readDatum(item)) return false;
            out.items.push_back(std::move(item));
        }
        if (!error_) fail(kind == datumKind::list ? "unclosed (" : "unclosed [", spanOf(open));
        return false;
    }

    std::string_view src_;
    std::vector<token> tokens_;
    std::size_t pos_ = 0;
    int depth_ = 0;
    std::optional<readError> error_;
};

} // namespace

readResult read(std::string_view source) {
    lexResult lexed = lexStrict(source);
    if (lexed.failure) {
        readResult result;
        result.error =
            readError{std::string(describe(lexed.failure->error)), spanOf(*lexed.failure)};
        return result;
    }
    return reader(source, std::move(lexed.tokens)).run();
}

} // namespace interpreter
