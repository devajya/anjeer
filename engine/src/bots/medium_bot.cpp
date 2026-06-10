#include "engine/bots/medium_bot.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace anjeer::engine {

// ── Helpers ───────────────────────────────────────────────────────────────────

void MediumBot::clear_pending() {
    for (int s = 0; s < 4; ++s)
        for (int sd = 0; sd < 2; ++sd)
            pending_orders_[s][sd] = std::nullopt;
}

float MediumBot::ev(int suit_idx) const {
    return base_ev(deck_weights_, suit_idx, player_count_);
}

// ── Events ────────────────────────────────────────────────────────────────────

void MediumBot::on_event(const BotEvent& event) {
    std::visit([this](const auto& e) { handle(e); }, event);
}

void MediumBot::handle(const BotRoundStartEvent& e) {
    player_slot_       = e.player_slot;
    player_count_      = e.player_count;
    balance_           = e.balance;
    hand_              = e.hand;
    round_active_      = true;
    has_fill_          = false;
    own_fill_cooldown_ = 0;
    clear_pending();
    deck_weights_ = compute_hand_posterior(hand_);
}

void MediumBot::handle(const BotTradeEvent& e) {
    clear_pending();

    if (e.your_side) {
        int si = suit_index(e.suit);
        if (*e.your_side == Side::Buy) {
            hand_[si]++;
            balance_ -= e.price;
        } else {
            hand_[si] = std::max(0, hand_[si] - 1);
            balance_ += e.price;
        }
        // Own trade: suppress belief signal for this fill and the next 4 observed fills.
        own_fill_cooldown_ = 5;
    }

    // Belief update: skip during cooldown, decrement after each fill.
    if (own_fill_cooldown_ > 0) {
        --own_fill_cooldown_;
        return;
    }

    // Noisy Bayesian update on observed fills (not own trades).
    std::uniform_real_distribution<float> noise(0.8f, 1.2f);
    int s = suit_index(e.suit);
    for (int d = 0; d < 12; ++d) {
        float L = (DECK_TABLE[d].goal_suit_index == s)
                  ? (static_cast<float>(e.price) / 10.0f)
                  : std::max(0.1f, 1.0f - static_cast<float>(e.price) / 10.0f);
        deck_weights_[d] *= L * noise(rng_);
    }
    renormalise(deck_weights_);
    has_fill_ = true;
}

void MediumBot::handle(const BotOrderAckEvent& e) {
    int si  = suit_index(e.suit);
    int sdi = side_idx(e.side);
    pending_orders_[si][sdi] = BotPendingOrder{e.order_id, e.suit, e.side, e.price, clock::now()};
}

// ── Taker scan ────────────────────────────────────────────────────────────────

std::vector<BotAction> MediumBot::taker_scan(const GameStateSnapshot& snap) const {
    float best_edge = 0.0f;
    std::optional<BotAction> best;

    for (int s = 0; s < 4; ++s) {
        float ev_s = ev(s);
        if (ev_s < 0.5f) continue;

        // Buy: skip if my own resting sell is at exactly the best ask price.
        // Prefer crossing when a large resting offer exists (qty >= 3 → +0.05 threshold).
        if (snap.best_ask[s] && snap.hand[s] < cfg_.hand_size_cap
                && snap.balance >= *snap.best_ask[s]) {
            const auto& my_ask = pending_orders_[s][1];
            if (!my_ask || my_ask->price != *snap.best_ask[s]) {
                float eff_threshold = cfg_.taker_threshold;
                if (snap.best_ask_qty[s] && *snap.best_ask_qty[s] >= 3)
                    eff_threshold += 0.05f;
                float ratio = static_cast<float>(*snap.best_ask[s]) / ev_s;
                if (ratio <= eff_threshold) {
                    float edge = ev_s - static_cast<float>(*snap.best_ask[s]);
                    if (edge > best_edge) {
                        best_edge = edge;
                        best = BotSubmitOrder{kAllSuits[s], Side::Buy, *snap.best_ask[s]};
                    }
                }
            }
        }

        // Sell: skip if my own resting buy is at exactly the best bid price.
        if (snap.best_bid[s] && snap.hand[s] >= 1) {
            const auto& my_bid = pending_orders_[s][0];
            if (!my_bid || my_bid->price != *snap.best_bid[s]) {
                float ratio = static_cast<float>(*snap.best_bid[s]) / ev_s;
                if (ratio >= (2.0f - cfg_.taker_threshold)) {
                    float edge = static_cast<float>(*snap.best_bid[s]) - ev_s;
                    if (edge > best_edge) {
                        best_edge = edge;
                        best = BotSubmitOrder{kAllSuits[s], Side::Sell, *snap.best_bid[s]};
                    }
                }
            }
        }
    }

    if (best) return {*best};
    return {};
}

// ── Resting order review ──────────────────────────────────────────────────────

std::vector<BotAction> MediumBot::review_pending(const GameStateSnapshot& snap) {
    std::vector<BotAction> actions;
    auto now = clock::now();

    bool endgame = (cfg_.endgame_threshold_s > 0 &&
                    snap.time_remaining_s < static_cast<float>(cfg_.endgame_threshold_s));
    int eff_resting_ms = endgame ? cfg_.max_resting_ms / 2 : cfg_.max_resting_ms;

    auto P      = goal_posteriors(deck_weights_);
    int  goal_s = (int)(std::max_element(P.begin(), P.end()) - P.begin());

    for (int s = 0; s < 4; ++s) {
        for (int sd = 0; sd < 2; ++sd) {
            auto& entry = pending_orders_[s][sd];
            if (!entry) continue;

            // Endgame: cancel non-goal asks immediately to free up inventory.
            if (endgame && sd == 1 && s != goal_s) {
                actions.push_back(BotCancelOrder{entry->order_id});
                entry = std::nullopt;
                continue;
            }

            auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - entry->placed_at).count();
            float ev_s = ev(s);
            float gap  = std::abs(ev_s - static_cast<float>(entry->price));

            if (age_ms > eff_resting_ms) {
                actions.push_back(BotCancelOrder{entry->order_id});
                entry = std::nullopt;
                continue;
            }

            if (gap > static_cast<float>(cfg_.nudge_max_gap)) {
                actions.push_back(BotCancelOrder{entry->order_id});
                entry = std::nullopt;
                continue;
            }

            if (age_ms > cfg_.nudge_patience_ms) {
                std::uniform_real_distribution<float> roll(0.0f, 1.0f);
                if (roll(rng_) < cfg_.nudge_probability) {
                    Side    side   = entry->side;
                    int32_t new_px = (side == Side::Buy) ? entry->price + 1 : entry->price - 1;
                    new_px = std::clamp(new_px, int32_t{1}, int32_t{20});
                    bool ok = (side == Side::Buy) ? (snap.balance >= new_px) : (snap.hand[s] >= 1);
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

std::vector<BotAction> MediumBot::gap_fill(const GameStateSnapshot& snap) const {
    bool endgame = (cfg_.endgame_threshold_s > 0 &&
                    snap.time_remaining_s < static_cast<float>(cfg_.endgame_threshold_s));

    auto  P      = goal_posteriors(deck_weights_);
    int   goal_s = (int)(std::max_element(P.begin(), P.end()) - P.begin());
    bool  conv   = (cfg_.conviction_threshold > 0.0f && P[goal_s] > cfg_.conviction_threshold);

    int resting = 0;
    for (int s = 0; s < 4; ++s)
        for (int sd = 0; sd < 2; ++sd)
            if (pending_orders_[s][sd]) resting++;

    int capacity = cfg_.max_concurrent_orders - resting;
    if (capacity <= 0) return {};

    struct Candidate { BotAction action; float priority; };
    std::vector<Candidate> cands;

    for (int s = 0; s < 4; ++s) {
        float ev_s   = ev(s);
        bool  s_goal = (s == goal_s);
        bool  s_conv = (conv && s_goal);

        if (endgame) {
            // Endgame: only bid goal suit at full EV.
            if (s_goal && !pending_orders_[s][0]) {
                int32_t price = std::clamp((int32_t)std::floor(ev_s), int32_t{1}, int32_t{20});
                if (snap.balance >= price)
                    cands.push_back({BotSubmitOrder{kAllSuits[s], Side::Buy, price}, ev_s});
            }
        } else {
            // Bid slot. Widen margin on thin books (ask qty absent or ≤ 1)
            // to reflect higher uncertainty in low-liquidity markets.
            if (!pending_orders_[s][0] && ev_s > cfg_.min_bid_ev
                    && snap.hand[s] < cfg_.hand_size_cap) {
                bool thin_book = !snap.best_ask_qty[s] || *snap.best_ask_qty[s] <= 1;
                float eff_discount = cfg_.confidence_discount - (thin_book ? 0.05f : 0.0f);
                int32_t price = s_conv
                    ? std::clamp((int32_t)std::floor(ev_s), int32_t{1}, int32_t{20})
                    : std::clamp((int32_t)std::floor(ev_s * eff_discount), int32_t{1}, int32_t{20});
                if (snap.balance >= price)
                    cands.push_back({BotSubmitOrder{kAllSuits[s], Side::Buy, price},
                                     std::abs(ev_s - (float)price)});
            }

            // Ask slot.
            if (!pending_orders_[s][1] && snap.hand[s] >= 1) {
                if (conv && !s_goal) {
                    // Conviction: dump non-goal at minimum price.
                    cands.push_back({BotSubmitOrder{kAllSuits[s], Side::Sell, 1}, ev_s});
                } else if (ev_s < cfg_.max_ask_ev || snap.hand[s] > cfg_.offload_threshold) {
                    int32_t price = std::clamp(
                        (int32_t)std::ceil(ev_s * (2.0f - cfg_.confidence_discount)), int32_t{1}, int32_t{20});
                    cands.push_back({BotSubmitOrder{kAllSuits[s], Side::Sell, price},
                                     std::abs(ev_s - (float)price)});
                }
            }
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

// ── Passive quoting ───────────────────────────────────────────────────────────

std::vector<BotAction> MediumBot::passive_quote(const GameStateSnapshot& snap) {
    if (cfg_.quoting_kappa <= 0.0f) return {};
    std::uniform_real_distribution<float> roll(0.0f, 1.0f);
    if (roll(rng_) >= cfg_.quoting_kappa) return {};

    int   best_s  = 0;
    float best_ev = ev(0);
    for (int s = 1; s < 4; ++s) { float e = ev(s); if (e > best_ev) { best_ev = e; best_s = s; } }

    int32_t bid_px = std::clamp((int32_t)std::floor(best_ev) - 1, int32_t{1}, int32_t{20});
    int32_t ask_px = std::clamp((int32_t)std::ceil(best_ev)  + 1, int32_t{1}, int32_t{20});

    std::vector<BotAction> actions;
    if (!pending_orders_[best_s][0] && snap.balance >= bid_px
            && snap.hand[best_s] < cfg_.hand_size_cap)
        actions.push_back(BotSubmitOrder{kAllSuits[best_s], Side::Buy,  bid_px});
    if (!pending_orders_[best_s][1] && snap.hand[best_s] >= 1 && ask_px > bid_px)
        actions.push_back(BotSubmitOrder{kAllSuits[best_s], Side::Sell, ask_px});
    return actions;
}

// ── decide() ──────────────────────────────────────────────────────────────────

std::vector<BotAction> MediumBot::decide(const GameStateSnapshot& snap) {
    if (!snap.round_active) return {};

    auto taker = taker_scan(snap);
    if (!taker.empty()) return taker;

    auto review = review_pending(snap);
    auto fill   = gap_fill(snap);
    review.insert(review.end(), fill.begin(), fill.end());

    if (review.empty()) {
        auto quote = passive_quote(snap);
        review.insert(review.end(), quote.begin(), quote.end());
    }
    return review;
}

// ── Debug ─────────────────────────────────────────────────────────────────────

std::string MediumBot::debug_info(const GameStateSnapshot& /*snap*/) const {
    static const char* sn[] = {"C", "D", "H", "S"};
    auto P = goal_posteriors(deck_weights_);
    int  g = 0;
    for (int s = 1; s < 4; ++s) if (P[s] > P[g]) g = s;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "goal=" << sn[g] << " P=[";
    for (int s = 0; s < 4; ++s)
        ss << sn[s] << ":" << P[s] << (s < 3 ? " " : "]");
    ss << " hand=[" << hand_[0] << " " << hand_[1] << " " << hand_[2] << " " << hand_[3] << "]"
       << " fill=" << (has_fill_ ? "yes" : "no")
       << " cd=" << own_fill_cooldown_;
    return ss.str();
}

std::unique_ptr<MediumBot> make_medium_bot(const BotConfig& cfg, uint64_t seed) {
    return std::make_unique<MediumBot>(cfg, seed);
}

} // namespace anjeer::engine
