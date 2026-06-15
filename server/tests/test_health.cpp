#include <catch2/catch_test_macros.hpp>

TEST_CASE("GET /health returns 200 when DB is reachable", "[health][integration]") {
    REQUIRE(false);  // stub — fails until implementation
}

TEST_CASE("GET /health returns 503 when DB pool fails SELECT 1", "[health][integration]") {
    REQUIRE(false);  // stub — fails until implementation
}
