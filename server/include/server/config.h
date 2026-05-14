#pragma once

#include <string>
#include <vector>

namespace anjeer::server {

// AGENT-CTX: All configurable values live here — never use literals for thresholds,
// ports, or intervals anywhere in application code. Tests pass a test.json override
// to the server binary via --config to avoid touching production defaults.
struct ServerConfig {
    std::string host;
    int         port;
    int         heartbeat_interval_ms;
    // AGENT-CTX: uWebSockets enforces a minimum idleTimeout of 8 seconds (hard
    // requirement in the library; values below 8 are silently clamped or rejected).
    // ping_timeout_ms / 1000 is passed as idleTimeout, so ping_timeout_ms must be
    // ≥ 8000. The "disconnection within 3 seconds" AC is satisfied by the client-side
    // heartbeat monitor (useWebSocket.ts), not by the server-side ping/pong timeout.
    // load_config() asserts ping_timeout_ms >= 8000 at startup.
    int         ping_interval_ms;
    int         ping_timeout_ms;

    // AGENT-CTX: OrderBookConfig mirrors the engine's OrderBook::Config but is parsed
    // from JSON by the server. The server constructs one OrderBook::Config per active
    // suit from these values. active_suits is a list so Slice 3 just extends it to
    // ["S1","S2","S3","S4"] with no changes to this struct or the parser.
    struct OrderBookConfig {
        int32_t                  min_price;
        int32_t                  max_price;
        // AGENT-CTX: nudge_initial_buy_price / nudge_initial_sell_price are the
        // starting prices when a player nudges with no resting orders on that side.
        // Kept in config (not hardcoded) so game designers can tune the opening
        // spread without a code change.
        int32_t                  nudge_initial_buy_price;
        int32_t                  nudge_initial_sell_price;
        // AGENT-CTX: active_suits drives how many OrderBook instances the server
        // creates. Slice 2 = ["S1"]. Slice 3 replaces this with the four real suit
        // names. The server validates inbound orders against this list and returns
        // UNKNOWN_SUIT for anything not in it.
        std::vector<std::string> active_suits;
    } order_book;

    // AGENT-CTX: Lobby seam — player_count is hardcoded here for Slice 3.
    // Replaced by LobbyConfig in the lobby slice.
    struct GameConfig {
        int                  player_count;
        int                  total_cards;
        int                  countdown_seconds;
        int                  round_duration_seconds;
        int                  inter_round_seconds;
    } game;

    // AGENT-CTX: ScoringConfig is the server-side view of scoring constants.
    // pot_size is the fixed total pot per round ($200 by default). Each player antes
    // pot_size / player_count at round start. bonus_pool for the engine is derived as
    // pot_size − total_goal_cards × points_per_card (computed in GameSession::end_round).
    // starting_balance is server-only state — the engine receives pre-buyin balances
    // as a vector and never tracks balances across rounds.
    struct ScoringConfig {
        int starting_balance;
        int pot_size;
        int points_per_card;
    } scoring;

    // AGENT-CTX: http_port is the Crow REST server port (8080); port above is the
    // uWebSockets game port (9001). Both run in the same process on separate threads.
    // cors_origin is the single allowed CORS origin — single-origin for now since
    // the frontend is always served from one place. Extend to vector in Slice 15 if
    // a CDN origin is added.
    int         http_port;
    std::string cors_origin;

    struct DbConfig {
        std::string connection_string;
        int         pool_size;
        std::string migrations_dir;
    } db;

    // AGENT-CTX: OAuthProviderConfig holds credentials for a single OAuth2 provider.
    // client_id and client_secret come from the provider's developer console — they
    // must NOT be committed; set them in config/dev.json (gitignored) at dev time
    // and in AWS SSM Parameter Store at production time (Slice 16).
    // redirect_uri must exactly match what is registered in the provider's app settings.
    struct AuthConfig {
        std::string jwt_secret;
        int         access_token_ttl_seconds;
        int         refresh_token_ttl_seconds;
        int         spectate_token_ttl_minutes;
        bool        secure_cookies;

        struct OAuthProviderConfig {
            std::string client_id;
            std::string client_secret;
            std::string redirect_uri;
        };
        OAuthProviderConfig github;
        OAuthProviderConfig google;
    } auth;

    struct LobbyConfig {
        int min_players;
        int max_players;
    } lobby;

    std::string event_bus;  // "local" | "redis" (redis = Slice 16)

    struct BotsConfig {
        int  scheduler_threads    = 4;
        int  scheduler_tick_ms    = 150;  // global BotScheduler fire rate; per-bot decide rate is in PerDifficultyParams
        int  sim_network_delay_ms = 50;
        bool spawn_bots_on_leave  = false;

        struct PerDifficultyParams {
            int   tick_interval_ms;       // base decide cadence
            int   tick_jitter_ms;         // ± random jitter on top of base interval
            int   thinking_min_ms;        // per-action delay lower bound
            int   thinking_max_ms;        // per-action delay upper bound
            float confidence_discount;    // bid = floor(EV * discount), ask = ceil(EV * (2-discount))
            float taker_threshold;        // cross if ask/EV <= threshold (or bid/EV >= 2-threshold)
            float min_bid_ev;             // don't bid a suit below this EV
            float max_ask_ev;             // don't ask a suit above this EV (unless offload_threshold hit)
            int   hand_size_cap;          // stop buying a suit once hand count reaches this
            int   offload_threshold;      // always ask if hand count exceeds this
            int   max_concurrent_orders;  // max resting orders allowed simultaneously
            float conviction_threshold;   // P(goal=s) above which conviction mode activates
            int   max_resting_ms;         // cancel resting order after this age
            float nudge_probability;      // per-tick chance to nudge a stale order by 1 tick
            int   nudge_patience_ms;      // wait this long before nudging
            int   nudge_max_gap;          // cancel-repost if |EV - price| exceeds this
            int   endgame_threshold_s;    // seconds remaining at which endgame mode activates
            float early_seed_threshold;   // hard only: seed bid when max(P) < this in first half
        } easy, medium, hard;
    } bots;

    // ARCHITECTURE-NOTE: in-process only; cross-node limiting deferred to Slice 16.
    struct RateLimitConfig {
        double  capacity          = 20.0;
        double  refill_rate       = 5.0;
        int32_t suspend_threshold = 50;
        int32_t suspend_seconds   = 30;
    } rate_limit;
};

// Load and parse a JSON config file.
// Throws std::runtime_error if the file cannot be opened or the JSON is malformed
// or missing required fields.
// AGENT-CTX: No defaults are applied here — every field must be present in the file.
// This makes misconfiguration loudly visible rather than silently falling back.
ServerConfig load_config(const std::string& path);

} // namespace anjeer::server
