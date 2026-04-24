#pragma once

#include <array>
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
        std::array<int, 4>   card_distribution;
        int                  countdown_seconds;
        int                  round_duration_seconds;
    } game;

    // AGENT-CTX: ScoringConfig is the server-side view of scoring constants.
    // It mirrors engine::ScoringConfig (buy_in, points_per_card) but adds
    // starting_balance, which is server-only state — the engine receives
    // pre-buyin balances as a vector and never tracks balances across rounds.
    // Keeping it here (not in the engine) maintains the engine's I/O-free invariant.
    struct ScoringConfig {
        int starting_balance;
        int buy_in;
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
};

// Load and parse a JSON config file.
// Throws std::runtime_error if the file cannot be opened or the JSON is malformed
// or missing required fields.
// AGENT-CTX: No defaults are applied here — every field must be present in the file.
// This makes misconfiguration loudly visible rather than silently falling back.
ServerConfig load_config(const std::string& path);

} // namespace anjeer::server
