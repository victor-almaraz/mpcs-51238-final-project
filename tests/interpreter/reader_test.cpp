#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <proj/interpreter/lexer.h>
#include <proj/interpreter/reader.h>

#include "support/unwrap.h"

using interpreter::datumKind;

TEST_CASE("reader builds nested data", "[interpreter][reader]") {
    const auto r = interpreter::read(R"((svf :in osc [0 1] "a\nb" 'x) 42)");
    REQUIRE_FALSE(r.error);
    REQUIRE(r.data.size() == 2);
    const auto& list = r.data[0];
    CHECK(list.kind == datumKind::list);
    REQUIRE(list.items.size() == 6);
    CHECK(list.items[0].isSymbol("svf"));
    CHECK(list.items[1].kind == datumKind::keyword);
    CHECK(list.items[1].text == "in");
    CHECK(list.items[3].kind == datumKind::vector);
    CHECK(list.items[3].items.size() == 2);
    CHECK(list.items[4].text == "a\nb");
    const auto& quoted = list.items[5];
    REQUIRE(quoted.items.size() == 2);
    CHECK(quoted.items[0].isSymbol("quote"));
    CHECK(quoted.items[1].isSymbol("x"));
    CHECK(r.data[1].integer == 42);
}

TEST_CASE("reader records spans", "[interpreter][reader]") {
    const auto r = interpreter::read("  (a\n   b)");
    REQUIRE_FALSE(r.error);
    const auto& list = r.data[0];
    CHECK(list.span.offset == 2);
    CHECK(list.span.length == 8);
    CHECK(list.items[1].span.line == 2);
    CHECK(list.items[1].span.column == 4);
}

TEST_CASE("reader reads unit literals as quantities", "[interpreter][reader]") {
    const auto r = interpreter::read("250ms -6db");
    REQUIRE_FALSE(r.error);
    CHECK(r.data[0].kind == datumKind::quantity);
    CHECK_THAT(r.data[0].quantity.value, Catch::Matchers::WithinRel(250.0));
    CHECK(r.data[0].quantity.unit == interpreter::unitSuffix::ms);
    CHECK(r.data[1].quantity.unit == interpreter::unitSuffix::db);
}

TEST_CASE("reader skips comments and datum comments", "[interpreter][reader]") {
    const auto r = interpreter::read("; c\n#| b |# a #;(b c) d");
    REQUIRE_FALSE(r.error);
    REQUIRE(r.data.size() == 2);
    CHECK(r.data[0].isSymbol("a"));
    CHECK(r.data[1].isSymbol("d"));
}

TEST_CASE("reader reports structural and lexical errors", "[interpreter][reader]") {
    CHECK(support::unwrap(interpreter::read("(a b").error).message == "unclosed (");
    CHECK(support::unwrap(interpreter::read("(a ]").error).message == "mismatched closing delimiter");
    CHECK(support::unwrap(interpreter::read(")").error).span.offset == 0);
    CHECK(support::unwrap(interpreter::read("(a 2x)").error).span.offset == 3);
}
