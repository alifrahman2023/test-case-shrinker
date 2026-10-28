#include "version.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("The core identifies this release without CLI configuration", "[core]") {
    REQUIRE(shrink::version() == "0.1.0");
}
