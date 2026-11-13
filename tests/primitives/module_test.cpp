#include <catch2/catch_test_macros.hpp>

#include <proj/primitives/module.h>

TEST_CASE("primitives module links", "[primitives]") {
    CHECK(primitives::moduleName() == "primitives");
}
