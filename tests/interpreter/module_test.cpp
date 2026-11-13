#include <catch2/catch_test_macros.hpp>

#include <proj/interpreter/module.h>

TEST_CASE("interpreter module links", "[interpreter]") {
    CHECK(interpreter::moduleName() == "interpreter");
}
