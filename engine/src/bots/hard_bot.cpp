#include "engine/bots/hard_bot.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace anjeer::engine {

// ── Helpers ───────────────────────────────────────────────────────────────────

void HardBot::clear_pending() {
    for (int s = 0; s < 4; ++s)
        for (int sd = 0; sd < 2; ++sd)
            pending_orders_[s][sd] = std::nullopt;
}

float HardBot::ev(int suit_idx) const {
    return base_ev(deck_weights_, suit_idx, player_count_);
}

void HardBot::check_lock_in() {
    if (goal_suit_locked_) return;

    float max_w = *std::max_element(deck_weights_.begin(), deck_weights_.end());
    if (max_w > 0.95f) { trigger_lock_in(); return; }

    for (int s = 0; s < 4; ++s) {
        if (observed_suit_counts_[s] > 8) {
            for (int d = 0; d < 12; ++d)
                if (DECK_TABLE[d].suit_counts[s] == 8) deck_weights_[d] = 0.0f;
            renormalise(deck_weights_);
            max_w = *std::max_element(deck_weights_.begin(), deck_weights_.end());
            if (max_w > 0.95f) { trigger_lock_in(); return; }
        }
    }
}

void HardBot::trigger_lock_in() {
    goal_suit_locked_ = true;
    auto P = goal_posteriors(deck_weights_);
    locked_goal_suit_ = (int)(std::max_element(P.begin(), P.end()) - P.begin());
}

// ── Events ────────────────────────────────────────────────────────────────────

void HardBot::on_event(const BotEvent& event) {
    std::visit([this](const auto& e) { handle(e); }, event);
}

void HardBot::handle(const BotRoundStartEvent& e) {
    player_slot_          = e.player_slot;
    player_count_         = e.player_count;
    balance_              = e.balance;
    hand_                 = e.hand;
    round_active_         = true;
    has_fill_             = false;
    own_fill_cooldown_    = 0;
    goal_suit_locked_     = false;
    locked_goal_suit_     = -1;
    observed_suit_counts_ = {};
    last_trade_price_.fill(std::nullopt);
    prev_best_bid_.fill(std::nullopt);
    prev_best_ask_.fill(std::nullopt);
    player_holdings_.assign(player_count_, {});
    player_pressure_.assign(player_count_, {});
    clear_pending();
    deck_weights_ = compute_hand_posterior(hand_);
}

void HardBot::handle(const BotTradeEvent& e) {
    clear_pending();

    int si = suit_index(e.suit);

    if (e.your_side) {
        if (*e.your_side == Side::Buy) {
            hand_[si]++;
            balance_ -= e.price;
        } else {
            hand_[si] = std::max(0, hand_[si] - 1);
            balance_ += e.price;
        }
        // Own trade: suppress fill signal for this fill and the next 4 observed fills.
        own_fill_cooldown_ = 5;
    }

    last_trade_price_[si]    = e.price;
    observed_suit_counts_[si]++;

    // Belief update: skip during cooldown.
    if (own_fill_cooldown_ > 0) {
        --own_fill_cooldown_;
        has_fill_ = true;
        check_lock_in();
        return;
    }

    // Exact Bayesian update (no noise).
    for (int d = 0; d < 12; ++d) {
        float L = (DECK_TABLE[d].goal_suit_index == si)
                  ? (static_cast<float>(e.price) / 10.0f)
                  : std::max(0.1f, 1.0f - static_cast<float>(e.price) / 10.0f);
        deck_weights_[d] *= L;
    }
    renormalise(deck_weights_);
    has_fill_ = true;
    check_lock_in();
}

void HardBot::handle(const BotBookUpdateEvent& e) {
    int si = suit_index(e.suit);

    auto& prev_bid = prev_best_bid_[si];
    auto& prev_ask = prev_best_ask_[si];

    bool new_aggressive_bid = e.best_bid && (!prev_bid || *e.best_bid > *prev_bid);
    bool new_aggressive_ask = e.best_ask && (!prev_ask || *e.best_ask < *prev_ask);

    if (new_aggressive_bid || new_aggressive_ask) {
        int32_t ref    = last_trade_price_[si].value_or(5);
        int32_t px     = new_aggressive_bid ? *e.best_bid : *e.best_ask;
        float   signal = std::min(0.5f, std::abs((float)(px - ref)) / 10.0f);

        for (int d = 0; d < 12; ++d) {
            if (DECK_TABLE[d].goal_suit_index == si && new_aggressive_bid)
                deck_weights_[d] *= (1.0f + signal);
            else
                deck_weights_[d] *= (1.0f - signal * 0.3f);
        }
        renormalise(deck_weights_);
    }

    prev_best_bid_[si] = e.best_bid;
    prev_best_ask_[si] = e.best_ask;
}

void HardBot::handle(const BotOrderAckEvent& e) {
    int si  = suit_index(e.suit);
    int sdi = side_idx(e.side);
    pending_orders_[si][sdi] = BotPendingOrder{e.order_id, e.suit, e.side, e.price, clock::now()};
}

// ── Taker scan ────────────────────────────────────────────────────────────────

std::vector<BotAction> HardBot::taker_scan(const GameStateSnapshot& snap) const {
    // Locked aggressive cross: when locked-in on the goal suit with a large resting
    // ask, cross even if ask > EV (negative edge) — certainty overrides the EV check.
    // This takes priority over the general taker scan below.
    if (goal_suit_locked_) {
        int gs = locked_goal_suit_;
        if (snap.best_ask[gs]
                && snap.best_ask_qty[gs] && *snap.best_ask_qty[gs] >= 3
                && snap.hand[gs] < cfg_.hand_size_cap
                && snap.balance >= *snap.best_ask[gs]) {
            const auto& my_ask = pending_orders_[gs][1];
            if (!my_ask || my_ask->price != *snap.best_ask[gs]) {
                float ev_s  = ev(gs);
                float ratio = static_cast<float>(*snap.best_ask[gs]) / ev_s;
                if (ratio <= cfg_.taker_threshold + 0.10f) {
                    int32_t room       = cfg_.hand_size_cap - snap.hand[gs];
                    int32_t affordable = snap.balance / *snap.best_ask[gs];
                    int32_t cross_qty  = std::min({*snap.best_ask_qty[gs], room, affordable, int32_t{4}});
                    if (cross_qty >= 1)
                        return {BotSubmitOrder{kAllSuits[gs], Side::Buy, *snap.best_ask[gs], cross_qty}};
                }
            }
        }
    }

    float best_edge = 0.0f;
    std::optional<BotAction> best;

    for (int s = 0; s < 4; ++s) {
        float ev_s = ev(s);
        if (ev_s < 0.5f) continue;

        // Buy: skip if my own resting sell is at exactly the best ask price.
        // Skip thin flash orders (qty == 1) — they may be bait.
        // Boost threshold for large resting offers (qty >= 3 → +0.05).
        if (snap.best_ask[s] && snap.hand[s] < cfg_.hand_size_cap
                && snap.balance >= *snap.best_ask[s]
                && !(snap.best_ask_qty[s] && *snap.best_ask_qty[s] == 1)) {
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

std::vector<BotAction> HardBot::review_pending(const GameStateSnapshot& snap) {
    std::vector<BotAction> actions;
    auto now = clock::now();

    bool endgame = (cfg_.endgame_threshold_s > 0 &&
                    snap.time_remaining_s < static_cast<float>(cfg_.endgame_threshold_s));
    int eff_resting_ms = endgame ? cfg_.max_resting_ms / 2 : cfg_.max_resting_ms;

    auto P      = goal_posteriors(deck_weights_);
    int  goal_s = goal_suit_locked_ ? locked_goal_suit_
                                    : (int)(std::max_element(P.begin(), P.end()) - P.begin());

    for (int s = 0; s < 4; ++s) {
        for (int sd = 0; sd < 2; ++sd) {
            auto& entry = pending_orders_[s][sd];
            if (!entry) continue;

            // Endgame: cancel non-goal asks immediately.
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

            // Cancel bids that have been jumped in queue: a better bid now rests
            // ahead of ours, so ours will be filled last (or not at all).
            if (sd == 0 && snap.best_bid[s] && *snap.best_bid[s] > entry->price) {
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
                    const auto& opp = pending_orders_[s][1 - sd];
                    bool crosses_own = opp && (
                        (side == Side::Buy  && new_px >= opp->price) ||
                        (side == Side::Sell && new_px <= opp->price));
                    if (ok && !crosses_own && new_px != entry->price) {
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

// ── Gap fill (pre-lock-in) ─────────────────────────────────────────────────────

std::vector<BotAction> HardBot::gap_fill(const GameStateSnapshot& snap) const {
    bool endgame = (cfg_.endgame_threshold_s > 0 &&
                    snap.time_remaining_s < static_cast<float>(cfg_.endgame_threshold_s));

    auto  P      = goal_posteriors(deck_weights_);
    int   goal_s = (int)(std::max_element(P.begin(), P.end()) - P.begin());
    // In endgame treat conviction as already met for the highest-posterior suit.
    bool  conv   = endgame || (cfg_.conviction_threshold > 0.0f && P[goal_s] > cfg_.conviction_threshold);

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
            // Bid slot.
            if (!pending_orders_[s][0] && ev_s > cfg_.min_bid_ev
                    && snap.hand[s] < cfg_.hand_size_cap) {
                int32_t price = s_conv
                    ? std::clamp((int32_t)std::floor(ev_s), int32_t{1}, int32_t{20})
                    : std::clamp((int32_t)std::floor(ev_s * cfg_.confidence_discount), int32_t{1}, int32_t{20});
                if (snap.balance >= price)
                    cands.push_back({BotSubmitOrder{kAllSuits[s], Side::Buy, price},
                                     std::abs(ev_s - (float)price)});
            }

            // Ask slot.
            if (!pending_orders_[s][1] && snap.hand[s] >= 1) {
                if (conv && !s_goal) {
                    int32_t dump_px  = std::max(int32_t{2}, (int32_t)std::floor(ev_s * 0.65f));
                    int32_t dump_qty = std::min(snap.hand[s], int32_t{2});
                    cands.push_back({BotSubmitOrder{kAllSuits[s], Side::Sell, dump_px, dump_qty}, ev_s});
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

// ── Locked actions (post-lock-in) ────────────────────────────────────────────

std::vector<BotAction> HardBot::locked_actions(const GameStateSnapshot& snap) const {
    int resting = 0;
    for (int s = 0; s < 4; ++s)
        for (int sd = 0; sd < 2; ++sd)
            if (pending_orders_[s][sd]) resting++;

    int capacity = cfg_.max_concurrent_orders - resting;
    if (capacity <= 0) return {};

    std::vector<BotAction> actions;

    // Bid goal suit at full EV, up to 2 cards at a time when locked in.
    if (!pending_orders_[locked_goal_suit_][0] &&
            snap.hand[locked_goal_suit_] < cfg_.hand_size_cap) {
        float   ev_s  = ev(locked_goal_suit_);
        int32_t price = std::clamp((int32_t)std::floor(ev_s), int32_t{1}, int32_t{20});
        if (snap.balance >= price) {
            int32_t room = cfg_.hand_size_cap - snap.hand[locked_goal_suit_];
            int32_t qty  = std::min({room, int32_t{2}, snap.balance / price});
            actions.push_back(BotSubmitOrder{kAllSuits[locked_goal_suit_], Side::Buy, price, std::max(int32_t{1}, qty)});
        }
    }

    // Ask non-goal suits in blocks (up to 3) at a discount to drain hand faster.
    for (int s = 0; s < 4; ++s) {
        if (s == locked_goal_suit_) continue;
        if (!pending_orders_[s][1] && snap.hand[s] >= 1) {
            int32_t price = std::max(int32_t{2}, (int32_t)std::floor(ev(s) * 0.65f));
            int32_t qty   = std::min(snap.hand[s], int32_t{3});
            actions.push_back(BotSubmitOrder{kAllSuits[s], Side::Sell, price, qty});
        }
    }

    if ((int)actions.size() > capacity) actions.resize(capacity);
    return actions;
}

// ── Seed market ───────────────────────────────────────────────────────────────

std::vector<BotAction> HardBot::seed_market(const GameStateSnapshot& snap) const {
    if (cfg_.early_seed_threshold <= 0.0f) return {};
    if (snap.time_remaining_s <= 0.5f * snap.round_duration_s) return {};

    auto  P     = goal_posteriors(deck_weights_);
    float max_p = *std::max_element(P.begin(), P.end());
    if (max_p >= cfg_.early_seed_threshold) return {};

    int best_s = (int)(std::max_element(P.begin(), P.end()) - P.begin());
    if (pending_orders_[best_s][0]) return {};

    float   ev_s  = ev(best_s);
    int32_t price = std::clamp((int32_t)std::floor(ev_s * 0.95f), int32_t{1}, int32_t{20});
    if (snap.balance < price) return {};
    return {BotSubmitOrder{kAllSuits[best_s], Side::Buy, price}};
}

// ── Passive quoting ───────────────────────────────────────────────────────────

std::vector<BotAction> HardBot::passive_quote(const GameStateSnapshot& snap) {
    if (cfg_.quoting_kappa <= 0.0f) return {};

    // Suppress during endgame lock-in.
    if (goal_suit_locked_ && cfg_.endgame_threshold_s > 0
            && snap.time_remaining_s < static_cast<float>(cfg_.endgame_threshold_s))
        return {};

    // Suppress when at max concurrent orders.
    int resting = 0;
    for (int s = 0; s < 4; ++s)
        for (int sd = 0; sd < 2; ++sd)
            if (pending_orders_[s][sd]) resting++;
    if (resting >= cfg_.max_concurrent_orders) return {};

    std::uniform_real_distribution<float> roll(0.0f, 1.0f);
    if (roll(rng_) >= cfg_.quoting_kappa) return {};

    int   best_s  = goal_suit_locked_ ? locked_goal_suit_ : 0;
    float best_ev = ev(0);
    if (!goal_suit_locked_) {
        for (int s = 1; s < 4; ++s) { float e = ev(s); if (e > best_ev) { best_ev = e; best_s = s; } }
    } else {
        best_ev = ev(best_s);
    }

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

std::vector<BotAction> HardBot::decide(const GameStateSnapshot& snap) {
    if (!snap.round_active) return {};

    auto taker = taker_scan(snap);
    if (!taker.empty()) return taker;

    auto review = review_pending(snap);

    std::vector<BotAction> fill;
    if (goal_suit_locked_)
        fill = locked_actions(snap);
    else
        fill = gap_fill(snap);

    auto seed = seed_market(snap);
    fill.insert(fill.end(), seed.begin(), seed.end());

    review.insert(review.end(), fill.begin(), fill.end());

    bool seen[4][2] = {};
    review.erase(std::remove_if(review.begin(), review.end(), [&](const BotAction& a) {
        if (const auto* sub = std::get_if<BotSubmitOrder>(&a)) {
            int si = suit_index(sub->suit), sdi = side_idx(sub->side);
            if (seen[si][sdi]) return true;
            seen[si][sdi] = true;
        }
        return false;
    }), review.end());

    if (review.empty()) {
        auto quote = passive_quote(snap);
        review.insert(review.end(), quote.begin(), quote.end());
    }
    return review;
}

// ── Debug ─────────────────────────────────────────────────────────────────────

std::string HardBot::debug_info(const GameStateSnapshot& /*snap*/) const {
    static const char* sn[] = {"C", "D", "H", "S"};
    auto P = goal_posteriors(deck_weights_);
    int  g = goal_suit_locked_ ? locked_goal_suit_
                                : (int)(std::max_element(P.begin(), P.end()) - P.begin());

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << (goal_suit_locked_ ? "LOCKED" : "goal") << "=" << sn[g] << " P=[";
    for (int s = 0; s < 4; ++s)
        ss << sn[s] << ":" << P[s] << (s < 3 ? " " : "]");
    ss << " hand=[" << hand_[0] << " " << hand_[1] << " " << hand_[2] << " " << hand_[3] << "]"
       << " cd=" << own_fill_cooldown_;
    return ss.str();
}

std::unique_ptr<HardBot> make_hard_bot(const BotConfig& cfg, uint64_t seed) {
    return std::make_unique<HardBot>(cfg, seed);
}

} // namespace anjeer::engine
