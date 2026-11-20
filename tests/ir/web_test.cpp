// Text lists and the web rows in the IR (io.md §5.1, §9.1, §9.3).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>

#include "support/pipes.h"
#include "support/unwrap.h"
#include "support/validation.h"

using Catch::Matchers::ContainsSubstring;
using metadata::graphSpec;
using metadata::paramValue;
using support::control;
using support::errors;
using strings = std::vector<std::string>;

namespace {

// A webIn polling @p url into a control pipe of @p channels channels,
// with @p params besides its URL.
graphSpec poll(std::map<std::string, paramValue, std::less<>> params, int channels = 2,
               std::string url = "https://api.example.com/now") {
    params.emplace("url", std::move(url));
    graphSpec g;
    g.pipes = {{.pipe = control(1), .channels = channels, .feedback = false}};
    g.nodes.push_back({.id = "w",
                       .type = "webIn",
                       .params = std::move(params),
                       .inputs = {},
                       .outputs = {{"value", {control(1)}}},
                       .src = std::nullopt});
    return g;
}

}  // namespace

TEST_CASE("a text list round trips as an array of strings and an empty one stays a text list") {
    const auto g = poll({{"fields", strings{"/main/temp", "/wind/speed"}}, {"headers", strings{}}});
    const auto text = ir::write(g);
    CHECK_THAT(text, ContainsSubstring(R"("fields":["/main/temp","/wind/speed"])"));
    CHECK_THAT(text, ContainsSubstring(R"("headers":[])"));
    const auto r = ir::read(text);
    INFO(r.text());
    REQUIRE(r.ok());
    CHECK(support::unwrap(r.graph) == g);
}

TEST_CASE("a text list holds strings only") {
    CHECK_THAT(errors(poll({{"fields", std::vector<double>{1.0, 2.0}}})),
               ContainsSubstring("parameter fields must be a list of strings"));
    CHECK_THAT(errors(poll({{"fields", std::string("/main/temp")}})),
               ContainsSubstring("parameter fields must be a list of strings"));
    // The schema refuses an array that mixes strings and numbers.
    auto text = ir::write(poll({{"fields", strings{"/a", "/b"}}}));
    text.replace(text.find(R"("/b")"), 4, "2");
    CHECK_THAT(ir::read(text).text(),
               ContainsSubstring("/nodes/0/params: validation failed for additional property 'fields'"));
}

TEST_CASE("webIn checks its URL fields headers and interval (io.md 9.3)") {
    CHECK(errors(poll({{"fields", strings{"/a", "/b"}}, {"headers", strings{"Accept: application/json"}}}))
              .empty());
    CHECK(errors(poll({}, 1, "http://localhost:8080/")).empty());
    CHECK_THAT(errors(poll({}, 1, "ftp://example.com/")),
               ContainsSubstring("parameter url must start with http://, https://"));
    CHECK_THAT(errors(poll({}, 1, "")), ContainsSubstring("parameter url must not be empty"));
    graphSpec noUrl = poll({});
    noUrl.nodes[0].params.erase("url");
    CHECK_THAT(errors(noUrl), ContainsSubstring("parameter url is required"));

    CHECK_THAT(errors(poll({{"fields", strings(9, "/a")}}, 8)),
               ContainsSubstring("parameter fields has 9 entries; at most 8 are allowed"));
    CHECK_THAT(errors(poll({{"fields", strings{"/a", "/b", "/c"}}}, 2)),
               ContainsSubstring("parameter fields has 3 entries for 2 channels on output value"));
    CHECK(errors(poll({}, 2)).empty());  // no fields reads the whole document

    for (const std::string bad : {"Accept", ": value", "Accept:value"}) {
        INFO(bad);
        CHECK_THAT(
            errors(poll({{"headers", strings{bad}}})),
            ContainsSubstring("parameter headers entry \"" + bad + "\" must be a header, Name: value"));
    }
    CHECK_THAT(errors(poll({{"every", 0.05}})),
               ContainsSubstring("parameter every is 0.05; expected 0.1 to 3600"));
}
