#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <utility>
#include <vector>

#include <espresso/workspace/highlighter.h>

using workspace::highlightCategory;
using C = highlightCategory;

namespace {

// Pairs of (text, category) for every span.
std::vector<std::pair<std::string_view, C>> categories(std::string_view src) {
    std::vector<std::pair<std::string_view, C>> out;
    for (const auto& s : workspace::highlight(src).spans) {
        out.emplace_back(src.substr(s.offset, s.length), s.category);
    }
    return out;
}

using expected = std::vector<std::pair<std::string_view, C>>;

}  // namespace

TEST_CASE("highlighter maps tokens to categories (tui.md 6.2)", "[workspace][highlighter]") {
    CHECK(categories("(define x 440) ; note") == expected{{"(", C::delimiter},
                                                          {"define", C::specialForm},
                                                          {"x", C::symbol},
                                                          {"440", C::number},
                                                          {")", C::delimiter},
                                                          {"; note", C::comment}});
    CHECK(categories("(defclass v () ((f :initform 250ms)))")[1] ==
          std::pair<std::string_view, C>{"defclass", C::objectForm});
    CHECK(categories(":freq \"a.sfz\" -6db 0.7 #t") == expected{{":freq", C::keyword},
                                                                {"\"a.sfz\"", C::string},
                                                                {"-6db", C::unitLiteral},
                                                                {"0.7", C::number},
                                                                {"#t", C::keyword}});
    CHECK(categories("#| block |# sine-osc") ==
          expected{{"#| block |#", C::comment}, {"sine-osc", C::symbol}});
}

TEST_CASE("highlighter handles incomplete input (tui.md 6.1)", "[workspace][highlighter]") {
    CHECK(categories("(play \"unterminated") ==
          expected{{"(", C::delimiter}, {"play", C::specialForm}, {"\"unterminated", C::error}});
    CHECK(categories("#| open") == expected{{"#| open", C::error}});
    CHECK(categories("2x (") == expected{{"2x", C::error}, {"(", C::delimiter}});
}

TEST_CASE("datum comments dim the next datum", "[workspace][highlighter]") {
    CHECK(categories("#;(a 1) b") == expected{{"#;", C::comment},
                                              {"(", C::comment},
                                              {"a", C::comment},
                                              {"1", C::comment},
                                              {")", C::comment},
                                              {"b", C::symbol}});
    CHECK(categories("#; 'x y")[2].second == C::comment);
    CHECK(categories("#; 'x y")[3].second == C::symbol);
}

TEST_CASE("matching delimiters", "[workspace][highlighter]") {
    const std::string_view src = "(a [b (c)] \")\")";
    const auto tokens = workspace::highlight(src).tokens;
    CHECK(workspace::matchingDelimiter(tokens, 0) == src.size() - 1);
    CHECK(workspace::matchingDelimiter(tokens, src.size() - 1) == 0u);
    CHECK(workspace::matchingDelimiter(tokens, 3) == 9u);
    CHECK_FALSE(workspace::matchingDelimiter(tokens, 1));
    CHECK_FALSE(workspace::matchingDelimiter(workspace::highlight("(a").tokens, 0));
}
