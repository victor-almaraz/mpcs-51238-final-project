#include <catch2/catch_test_macros.hpp>

#include <proj/metadata/module.h>

TEST_CASE("metadata module links", "[metadata]") {
    CHECK(metadata::moduleName() == "metadata");
}
