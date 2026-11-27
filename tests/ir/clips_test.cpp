// clipPlayer in the IR (canvas.md §5): a bank's name is a folder's, and one
// clipPlayer plays a bank.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <espresso/metadata/graph.h>

#include "support/pipes.h"
#include "support/validation.h"

using Catch::Matchers::ContainsSubstring;
using metadata::graphSpec;
using support::errors;
using support::events;

namespace {

// clipPlayers of banks @p banks, each writing an event pipe of its own.
graphSpec players(const std::vector<std::string>& banks) {
    graphSpec g;
    for (std::size_t i = 0; i < banks.size(); ++i) {
        const auto pipe = events(static_cast<int>(i));
        g.pipes.push_back({.pipe = pipe, .channels = 1, .feedback = false});
        g.nodes.push_back({.id = "clips-" + std::to_string(i),
                           .type = "clipPlayer",
                           .params = {{"bank", banks[i]}},
                           .inputs = {},
                           .outputs = {{"out", {pipe}}},
                           .src = std::nullopt});
    }
    return g;
}

}  // namespace

TEST_CASE("a clip bank's name is a folder's, and one clipPlayer plays it (canvas.md 5)", "[ir][clips]") {
    CHECK(errors(players({"bass", "lead-2"})).empty());
    CHECK_THAT(errors(players({"Bass"})), ContainsSubstring("lowercase letters, digits, and hyphens"));
    CHECK_THAT(errors(players({"../ui"})), ContainsSubstring("lowercase letters, digits, and hyphens"));
    CHECK_THAT(errors(players({"bass", "bass"})),
               ContainsSubstring("clip bank \"bass\" is played by another clipPlayer"));
}
