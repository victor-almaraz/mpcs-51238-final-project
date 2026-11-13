#include <catch2/catch_test_macros.hpp>

#include <proj/protocol/module.h>

TEST_CASE("protocol module links", "[protocol]") {
    CHECK(protocol::moduleName() == "protocol");
}
