#include "engine/bots/easy_bot.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace anjeer::engine {

// ── Helpers ───────────────────────────────────────────────────────────────────

void EasyBot::clear_pending() {
    for (int s = 0; s < 4; ++s)
        for (int sd = 0; sd < 2; ++sd)
            pending_orders_[s][sd] = std::nullopt;
}

float EasyBot::ev(int suit_idx) const {
    return base_ev(deck_weights_, suit_idx, player_count_);
}

// ── Events ────────────────────────────────────────────────────────────────────

void EasyBot::on_event(const BotEvent& event) {
    std::visit([this](const auto& e) { handle(e); }, event);
}

void EasyBot::handle(const BotRoundStartEvent& e) {
    player_count_ = e.player_count;
    balance_      = e.balance;
    hand_         = e.hand;
    round_active_ = true;
    clear_pending();

    // Find heaviest suit in hand → perceived 12-card suit.
    int perceived_12 = 0;
    for (int s = 1; s < 4; ++s)
        if (hand_[s] > hand_[perceived_12]) perceived_12 = s;

    // Correct inversion: same-colour partner is the perceived goal.
    // C(0)↔S(3) [black], D(1)↔H(2) [red]
    constexpr int kPartner[] = {3, 2, 1, 0};
    int perceived_goal = kPartner[perceived_12];

    // 0.80 mass to decks where goal=perceived_goal (3 decks),
    // 0.20 distributed equally to the 9 remaining decks.
    for (int d = 0; d < 12; ++d) {
        deck_weights_[d] = (DECK_TABLE[d].goal_suit_index == perceived_goal)
                           ? (0.80f / 3.0f)
                           : (0.20f / 9.0f);
    }
}

void EasyBot::handle(const BotTradeEvent& e) {
    // Global wipe: clear all resting orders.
    clear_pending();

    // Update own hand and balance if party to the fill.
    if (e.your_side) {
        int si = suit_index(e.suit);
        if (*e.your_side == Side::Buy) {
            hand_[si]++;
            balance_ -= e.price;
        } else {
            hand_[si] = std::max(0, hand_[si] - 1);
            balance_ += e.price;
        }
    }
    // Easy: no belief update on fills.
}

void EasyBot::handle(const BotOrderAckEvent& e) {
    int si  = suit_index(e.suit);
    int sdi = side_idx(e.side);
    pending_orders_[si][sdi] = BotPendingOrder{
        e.order_id, e.suit, e.side, e.price, clock::now()};
}

// ── Taker scan ────────────────────────────────────────────────────────────────

std::vector<BotAction> EasyBot::taker_scan(const GameStateSnapshot& snap) const {
    float                    best_edge = 0.0f;
    std::optional<BotAction> best;

    for (int s = 0; s < 4; ++s) {
        float ev_s = ev(s);
        if (ev_s < 0.5f) continue;

        // Buy: cross ask if within taker_threshold of EV, have balance, have room.
        // Skip if the best ask is our own resting sell order at that exact price.
        if (snap.best_ask[s] && snap.hand[s] < cfg_.hand_size_cap
                && snap.balance >= *snap.best_ask[s]) {
            const auto& my_ask = pending_orders_[s][1];
            if (!my_ask || my_ask->price != *snap.best_ask[s]) {
                float ratio = static_cast<float>(*snap.best_ask[s]) / ev_s;
                if (ratio <= cfg_.taker_threshold) {
                    float edge = ev_s - static_cast<float>(*snap.best_ask[s]);
                    if (edge > best_edge) { best_edge = edge; best = BotSubmitOrder{kAllSuits[s], Side::Buy,  *snap.best_ask[s]}; }
                }
            }
        }

        // Sell: cross bid if sufficiently above EV, have the card.
        // Skip if the best bid is our own resting buy order at that exact price.
        if (snap.best_bid[s] && snap.hand[s] >= 1) {
            const auto& my_bid = pending_orders_[s][0];
            if (!my_bid || my_bid->price != *snap.best_bid[s]) {
                float ratio = static_cast<float>(*snap.best_bid[s]) / ev_s;
                if (ratio >= (2.0f - cfg_.taker_threshold)) {
                    float edge = static_cast<float>(*snap.best_bid[s]) - ev_s;
                    if (edge > best_edge) { best_edge = edge; best = BotSubmitOrder{kAllSuits[s], Side::Sell, *snap.best_bid[s]}; }
                }
            }
        }
    }

    if (best) return {*best};
    return {};
}

// ── Resting order review ──────────────────────────────────────────────────────

std::vector<BotAction> EasyBot::review_pending(const GameStateSnapshot& snap) {
    std::vector<BotAction> actions;
    auto now = clock::now();

    for (int s = 0; s < 4; ++s) {
        for (int sd = 0; sd < 2; ++sd) {
            auto& entry = pending_orders_[s][sd];
            if (!entry) continue;

            auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - entry->placed_at).count();
            float ev_s = ev(s);
            float gap  = std::abs(ev_s - static_cast<float>(entry->price));

            // Cancel if expired.
            if (age_ms > cfg_.max_resting_ms) {
                actions.push_back(BotCancelOrder{entry->order_id});
                entry = std::nullopt;
                continue;
            }

            // Cancel and repost if EV has moved past nudge_max_gap.
            if (gap > static_cast<float>(cfg_.nudge_max_gap)) {
                actions.push_back(BotCancelOrder{entry->order_id});
                entry = std::nullopt;
                continue;
            }

            // Nudge by 1 tick toward the spread when patient enough.
            if (age_ms > cfg_.nudge_patience_ms) {
                std::uniform_real_distribution<float> roll(0.0f, 1.0f);
                if (roll(rng_) < cfg_.nudge_probability) {
                    Side    side   = entry->side;
                    int32_t new_px = (side == Side::Buy) ? entry->price + 1
                                                         : entry->price - 1;
                    new_px = std::clamp(new_px, int32_t{1}, int32_t{20});
                    bool ok = (side == Side::Buy) ? (snap.balance >= new_px)
                                                  : (snap.hand[s] >= 1);
                    if (ok && new_px != entry->price) {
                        actions.push_back(BotCancelOrder{entry->order_id});
                        entry = std::nullopt;
                        actions.push_back(BotSubmitOrder{kAllSuits[s], side, new_px});
                    }
                }
            }
        }
    }
    return actions;
}

// ── Gap fill ─────────────────────────────────────────────────────────────────

std::vector<BotAction> EasyBot::gap_fill(const GameStateSnapshot& snap) const {
    int resting = 0;
    for (int s = 0; s < 4; ++s)
        for (int sd = 0; sd < 2; ++sd)
            if (pending_orders_[s][sd]) resting++;

    int capacity = cfg_.max_concurrent_orders - resting;
    if (capacity <= 0) return {};

    struct Candidate { BotAction action; float priority; };
    std::vector<Candidate> cands;

    for (int s = 0; s < 4; ++s) {
        float ev_s = ev(s);

        // Effective resting bid: either an already-acked order or one we're about to add.
        int32_t effective_bid = pending_orders_[s][0] ? pending_orders_[s][0]->price : -1;

        // Bid slot. Skip if the opposite side (ask) is thick — a thick ask is
        // better handled by taker_scan; adding a passive bid only wastes a slot.
        constexpr int kThickQty = 3;
        if (!pending_orders_[s][0] && ev_s > cfg_.min_bid_ev
                && snap.hand[s] < cfg_.hand_size_cap
                && !(snap.best_ask_qty[s] && *snap.best_ask_qty[s] >= kThickQty)) {
            int32_t price = std::clamp(
                (int32_t)std::floor(ev_s * cfg_.confidence_discount), int32_t{1}, int32_t{20});
            // Guard: don't bid above our own resting ask (inter-tick cross).
            bool crosses_own_ask = pending_orders_[s][1] && price >= pending_orders_[s][1]->price;
            if (!crosses_own_ask && snap.balance >= price) {
                cands.push_back({BotSubmitOrder{kAllSuits[s], Side::Buy, price},
                                 std::abs(ev_s - (float)price)});
                effective_bid = price;
            }
        }

        // Ask slot — skip if the ask would cross our own bid (same-tick or inter-tick).
        // Also skip if the opposite side (bid) is thick.
        if (!pending_orders_[s][1] && snap.hand[s] >= 1
                && (ev_s < cfg_.max_ask_ev || snap.hand[s] > cfg_.offload_threshold)
                && !(snap.best_bid_qty[s] && *snap.best_bid_qty[s] >= kThickQty)) {
            int32_t price = std::clamp(
                (int32_t)std::ceil(ev_s * (2.0f - cfg_.confidence_discount)), int32_t{1}, int32_t{20});
            if (price > effective_bid)
                cands.push_back({BotSubmitOrder{kAllSuits[s], Side::Sell, price},
                                 std::abs(ev_s - (float)price)});
        }
    }

    std::sort(cands.begin(), cands.end(),
              [](const Candidate& a, const Candidate& b) { return a.priority > b.priority; });

    std::vector<BotAction> actions;
    for (const auto& c : cands) {
        if ((int)actions.size() >= capacity) break;
        actions.push_back(c.action);
    }
    return actions;
}

// ── decide() ──────────────────────────────────────────────────────────────────

std::vector<BotAction> EasyBot::decide(const GameStateSnapshot& snap) {
    if (!snap.round_active) return {};

    auto taker = taker_scan(snap);
    if (!taker.empty()) return taker;

    auto review = review_pending(snap);
    auto fill   = gap_fill(snap);

    review.insert(review.end(), fill.begin(), fill.end());
    return review;
}

// ── Debug ─────────────────────────────────────────────────────────────────────

std::string EasyBot::debug_info(const GameStateSnapshot& /*snap*/) const {
    static const char* sn[] = {"C", "D", "H", "S"};
    auto P = goal_posteriors(deck_weights_);
    int g  = 0;
    for (int s = 1; s < 4; ++s) if (P[s] > P[g]) g = s;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "goal=" << sn[g] << " P=[";
    for (int s = 0; s < 4; ++s)
        ss << sn[s] << ":" << P[s] << (s < 3 ? " " : "]");
    ss << " hand=[" << hand_[0] << " " << hand_[1] << " " << hand_[2] << " " << hand_[3] << "]";
    return ss.str();
}

} // namespace anjeer::engine
