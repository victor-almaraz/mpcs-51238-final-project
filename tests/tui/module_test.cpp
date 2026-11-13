#include <catch2/catch_test_macros.hpp>

#include <proj/tui/module.h>

TEST_CASE("tui module links", "[tui]") {
    CHECK(tui::moduleName() == "tui");
}
