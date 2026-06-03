#pragma once

#include <stdexcept>
#include <string_view>

namespace anjeer::server {

enum class FeedTier  { MBP1, MBPN, MBO };
enum class Encoding  { JSON, MsgPack };

inline FeedTier feed_tier_from_string(std::string_view s) {
    if (s == "mbp1") return FeedTier::MBP1;
    if (s == "mbpn") return FeedTier::MBPN;
    if (s == "mbo")  return FeedTier::MBO;
    throw std::invalid_argument("unknown feed tier: " + std::string(s));
}

inline const char* feed_tier_to_string(FeedTier t) {
    switch (t) {
        case FeedTier::MBP1: return "mbp1";
        case FeedTier::MBPN: return "mbpn";
        case FeedTier::MBO:  return "mbo";
    }
    return "mbp1";
}

}  // namespace anjeer::server
