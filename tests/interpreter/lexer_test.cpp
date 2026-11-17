#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <vector>

#include <espresso/interpreter/lexer.h>

#include "support/unwrap.h"

using interpreter::lexError;
using interpreter::tokenKind;

namespace {

std::vector<tokenKind> kinds(std::string_view src) {
    std::vector<tokenKind> out;
    for (const auto& t : interpreter::lexTolerant(src)) out.push_back(t.kind);
    return out;
}

interpreter::token only(std::string_view src) {
    const auto tokens = interpreter::lexTolerant(src);
    REQUIRE(tokens.size() == 1);
    return tokens[0];
}

} // namespace

TEST_CASE("lexer classifies atoms", "[interpreter][lexer]") {
    CHECK(only("440").kind == tokenKind::integer);
    CHECK(only("-3").kind == tokenKind::integer);
    CHECK(only("0.7").kind == tokenKind::floating);
    CHECK(only("1e3").kind == tokenKind::floating);
    CHECK(only("-2.5E-2").kind == tokenKind::floating);
    CHECK(only("\"piano.sfz\"").kind == tokenKind::string);
    CHECK(only("#t").kind == tokenKind::boolean);
    CHECK(only("#f").kind == tokenKind::boolean);
    CHECK(only("nil").kind == tokenKind::nil);
    CHECK(only(":freq").kind == tokenKind::keyword);
    CHECK(only("sine-osc").kind == tokenKind::symbol);
    CHECK(only("set!").kind == tokenKind::symbol);
    CHECK(only("-").kind == tokenKind::symbol);
    CHECK(only("x2").kind == tokenKind::symbol);
    CHECK(only("&rest").kind == tokenKind::symbol);
}

TEST_CASE("lexer reads unit literals (grammar.md 1.4)", "[interpreter][lexer]") {
    for (std::string_view s : {"250ms", "2s", "-6db", "440hz", "1.2khz", "96bpm", "3st", "10ct",
                               "1beat", "2bar", "64smp", "5us"}) {
        INFO(s);
        CHECK(only(s).kind == tokenKind::unitLiteral);
    }
    const auto t = only("250ms");
    CHECK(t.numericLength == 3);

    const auto bad = only("2x");
    CHECK(bad.kind == tokenKind::error);
    CHECK(bad.error == lexError::malformedNumber);
    CHECK(only("1.").error == lexError::malformedNumber);
}

TEST_CASE("lexer handles delimiters and comments", "[interpreter][lexer]") {
    CHECK(kinds("(a [b] 'c)") ==
          std::vector{tokenKind::leftParen, tokenKind::symbol, tokenKind::leftBracket,
                      tokenKind::symbol, tokenKind::rightBracket, tokenKind::quote,
                      tokenKind::symbol, tokenKind::rightParen});
    CHECK(kinds("a ; note\nb") ==
          std::vector{tokenKind::symbol, tokenKind::lineComment, tokenKind::symbol});
    CHECK(kinds("#| outer #| inner |# still |# x") ==
          std::vector{tokenKind::blockComment, tokenKind::symbol});
    CHECK(kinds("#;(skip me) y") ==
          std::vector{tokenKind::datumComment, tokenKind::leftParen, tokenKind::symbol,
                      tokenKind::symbol, tokenKind::rightParen, tokenKind::symbol});
}

TEST_CASE("tolerant lexer turns malformed input into error tokens", "[interpreter][lexer]") {
    CHECK(only("\"open").error == lexError::unterminatedString);
    CHECK(only("#| open").error == lexError::unterminatedBlockComment);
    CHECK(only("\"bad \\q\"").error == lexError::invalidEscape);
    CHECK(only(":").error == lexError::emptyKeyword);
    CHECK(only("#x").error == lexError::invalidHashSyntax);
    CHECK(only("@").error == lexError::unexpectedCharacter);

    // Lexing continues after an error.
    CHECK(kinds("(a @ b)") == std::vector{tokenKind::leftParen, tokenKind::symbol,
                                          tokenKind::error, tokenKind::symbol,
                                          tokenKind::rightParen});
}

TEST_CASE("strict lexer stops at the first error", "[interpreter][lexer]") {
    const auto r = interpreter::lexStrict("(a 2x b)");
    const auto& failure = support::unwrap(r.failure);
    CHECK(failure.error == lexError::malformedNumber);
    CHECK(failure.offset == 3);
    CHECK(r.tokens.size() == 2);

    CHECK_FALSE(interpreter::lexStrict("(a b)").failure);
}

TEST_CASE("lexer tracks lines and code-point columns", "[interpreter][lexer]") {
    const std::string_view src = "\"é\" x\n  y";
    const auto tokens = interpreter::lexTolerant(src);
    REQUIRE(tokens.size() == 3);
    CHECK(tokens[1].line == 1);
    CHECK(tokens[1].column == 5);
    CHECK(tokens[2].line == 2);
    CHECK(tokens[2].column == 3);
    CHECK(tokens[2].text(src) == "y");
}
