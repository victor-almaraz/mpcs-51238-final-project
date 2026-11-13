#include <catch2/catch_test_macros.hpp>

#include <proj/ir/module.h>

TEST_CASE("ir module links", "[ir]") {
    CHECK(ir::moduleName() == "ir");
}
