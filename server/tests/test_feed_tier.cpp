#include <catch2/catch_test_macros.hpp>

#include "server/feed_tier.h"

using namespace anjeer::server;

TEST_CASE("feed_tier_from_string: valid values", "[feed_tier]") {
    REQUIRE(feed_tier_from_string("mbp1") == FeedTier::MBP1);
    REQUIRE(feed_tier_from_string("mbpn") == FeedTier::MBPN);
    REQUIRE(feed_tier_from_string("mbo")  == FeedTier::MBO);
}

TEST_CASE("feed_tier_from_string: unknown value throws", "[feed_tier]") {
    REQUIRE_THROWS_AS(feed_tier_from_string("invalid"), std::invalid_argument);
    REQUIRE_THROWS_AS(feed_tier_from_string("MBP1"),    std::invalid_argument);
    REQUIRE_THROWS_AS(feed_tier_from_string(""),         std::invalid_argument);
}

TEST_CASE("feed_tier_to_string: round-trips", "[feed_tier]") {
    REQUIRE(std::string_view(feed_tier_to_string(FeedTier::MBP1)) == "mbp1");
    REQUIRE(std::string_view(feed_tier_to_string(FeedTier::MBPN)) == "mbpn");
    REQUIRE(std::string_view(feed_tier_to_string(FeedTier::MBO))  == "mbo");
}
