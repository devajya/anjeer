#include <catch2/catch_test_macros.hpp>
#include "server/spectate_token_repo.h"
#include "server/db.h"
#include <pqxx/pqxx>
#include <cstdlib>

TEST_CASE("SpectateTokenRepo::cleanup_expired removes expired tokens", "[spectate][db]") {
    REQUIRE(false);  // stub — fails until implementation
}

TEST_CASE("SpectateTokenRepo::cleanup_expired leaves valid tokens intact", "[spectate][db]") {
    REQUIRE(false);  // stub — fails until implementation
}
