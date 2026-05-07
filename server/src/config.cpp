#include "server/config.h"

#include <fstream>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>

namespace anjeer::server {

// AGENT-CTX: load_config is called once at startup — not a hot path.
// Perf rules (inlining, SIMD, allocation minimisation) do not apply here.
// Correctness and loud failure on bad config take priority.
ServerConfig load_config(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open config file: " + path);
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error(
            std::string("Config JSON parse error in ") + path + ": " + e.what());
    }

    // AGENT-CTX: Use .at() rather than [] so missing keys throw nlohmann::json::out_of_range,
    // which we catch and re-throw as a descriptive runtime_error.
    // Silently defaulting a missing field would hide configuration bugs.
    try {
        // AGENT-CTX: ServerConfig is constructed then populated field-by-field.
        // NRVO (Named Return Value Optimisation) eliminates the copy on return —
        // no std::move needed; adding it would suppress NRVO per cpp_performance_rules.
        ServerConfig cfg;

        const auto& srv = j.at("server");
        cfg.host                  = srv.at("host").get<std::string>();
        cfg.port                  = srv.at("port").get<int>();
        cfg.heartbeat_interval_ms = srv.at("heartbeat_interval_ms").get<int>();
        cfg.ping_interval_ms      = srv.at("ping_interval_ms").get<int>();
        cfg.ping_timeout_ms       = srv.at("ping_timeout_ms").get<int>();

        // uWebSockets requires idleTimeout >= 8 s; enforce that here so a bad
        // config fails loudly at startup rather than silently misbehaving.
        if (cfg.ping_timeout_ms < 8000) {
            throw std::runtime_error(
                "ping_timeout_ms must be >= 8000 (uWebSockets idleTimeout minimum is 8 s); "
                "got " + std::to_string(cfg.ping_timeout_ms));
        }

        const auto& ob = j.at("order_book");
        cfg.order_book.min_price               = ob.at("min_price").get<int32_t>();
        cfg.order_book.max_price               = ob.at("max_price").get<int32_t>();
        cfg.order_book.nudge_initial_buy_price  = ob.at("nudge_initial_buy_price").get<int32_t>();
        cfg.order_book.nudge_initial_sell_price = ob.at("nudge_initial_sell_price").get<int32_t>();
        cfg.order_book.active_suits             = ob.at("active_suits").get<std::vector<std::string>>();

        const auto& gm = j.at("game");
        cfg.game.player_count            = gm.at("player_count").get<int>();
        cfg.game.total_cards             = gm.at("total_cards").get<int>();
        cfg.game.countdown_seconds       = gm.at("countdown_seconds").get<int>();
        cfg.game.round_duration_seconds  = gm.at("round_duration_seconds").get<int>();
        cfg.game.inter_round_seconds     = gm.at("inter_round_seconds").get<int>();

        const auto& sc = j.at("scoring");
        cfg.scoring.starting_balance = sc.at("starting_balance").get<int>();
        cfg.scoring.round_buy_in_pct = sc.at("round_buy_in_pct").get<double>();
        cfg.scoring.points_per_card  = sc.at("points_per_card").get<int>();

        // AGENT-CTX: round_buy_in_pct must be in (0, 1]. A pct that makes round_buy_in()
        // exceed starting_balance puts a player into negative balance on round entry.
        // Equality (pct=1.0) is allowed — the player spends their entire balance to enter.
        if (cfg.scoring.round_buy_in_pct <= 0.0 || cfg.scoring.round_buy_in_pct > 1.0) {
            throw std::runtime_error(
                "scoring.round_buy_in_pct must be in (0, 1]; got " +
                std::to_string(cfg.scoring.round_buy_in_pct));
        }
        if (cfg.scoring.round_buy_in() > cfg.scoring.starting_balance) {
            throw std::runtime_error(
                "computed round_buy_in (" + std::to_string(cfg.scoring.round_buy_in()) +
                ") must be <= starting_balance (" +
                std::to_string(cfg.scoring.starting_balance) + ")");
        }

        cfg.http_port   = srv.at("http_port").get<int>();
        cfg.cors_origin = srv.at("cors_origin").get<std::string>();

        const auto& db = j.at("db");
        cfg.db.connection_string = db.at("connection_string").get<std::string>();
        cfg.db.pool_size         = db.at("pool_size").get<int>();
        cfg.db.migrations_dir    = db.at("migrations_dir").get<std::string>();

        const auto& au = j.at("auth");
        cfg.auth.jwt_secret = au.at("jwt_secret").get<std::string>();
        cfg.auth.access_token_ttl_seconds    = au.at("access_token_ttl_seconds").get<int>();
        cfg.auth.refresh_token_ttl_seconds   = au.at("refresh_token_ttl_seconds").get<int>();
        cfg.auth.spectate_token_ttl_minutes  = au.at("spectate_token_ttl_minutes").get<int>();
        cfg.auth.secure_cookies              = au.at("secure_cookies").get<bool>();

        // AGENT-CTX: OAuth provider parsing is identical for both providers.
        // A future slice could make providers a map<string, OAuthProviderConfig> to
        // support dynamic provider registration, but two hard-coded providers is
        // sufficient through Slice 9 (API mode). Do not generalise prematurely.
        auto parse_provider = [](const nlohmann::json& j)
            -> ServerConfig::AuthConfig::OAuthProviderConfig {
            return {
                j.at("client_id").get<std::string>(),
                j.at("client_secret").get<std::string>(),
                j.at("redirect_uri").get<std::string>()
            };
        };
        cfg.auth.github  = parse_provider(au.at("github"));
        cfg.auth.google  = parse_provider(au.at("google"));

        const auto& lb = j.at("lobby");
        cfg.lobby.min_players = lb.at("min_players").get<int>();
        cfg.lobby.max_players = lb.at("max_players").get<int>();

        cfg.event_bus = j.at("event_bus").get<std::string>();

        const auto& rl = j.at("rate_limit");
        cfg.rate_limit.capacity          = rl.at("capacity").get<double>();
        cfg.rate_limit.refill_rate       = rl.at("refill_rate").get<double>();
        cfg.rate_limit.suspend_threshold = rl.at("suspend_threshold").get<int32_t>();
        cfg.rate_limit.suspend_seconds   = rl.at("suspend_seconds").get<int32_t>();

        return cfg;
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(
            std::string("Config field error in ") + path + ": " + e.what());
    }
}

} // namespace anjeer::server
