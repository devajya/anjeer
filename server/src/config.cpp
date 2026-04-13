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

        // AGENT-CTX: order_book section added in Slice 2. All fields required —
        // omitting any throws here rather than silently defaulting, consistent
        // with the server section above.
        // int32_t is used for price bounds because width matters: these values
        // are passed directly to the engine's fixed-point integer price model.
        // If the engine ever widens to int64_t, widen here too.
        // active_suits is a JSON array of strings; nlohmann handles the conversion.
        // Slice 3 extends active_suits to four suit names — no struct change needed.
        const auto& ob = j.at("order_book");
        cfg.order_book.min_price               = ob.at("min_price").get<int32_t>();
        cfg.order_book.max_price               = ob.at("max_price").get<int32_t>();
        cfg.order_book.nudge_initial_buy_price  = ob.at("nudge_initial_buy_price").get<int32_t>();
        cfg.order_book.nudge_initial_sell_price = ob.at("nudge_initial_sell_price").get<int32_t>();
        cfg.order_book.active_suits             = ob.at("active_suits").get<std::vector<std::string>>();

        // uWebSockets requires idleTimeout >= 8 s; enforce that here so a bad
        // config fails loudly at startup rather than silently misbehaving.
        if (cfg.ping_timeout_ms < 8000) {
            throw std::runtime_error(
                "ping_timeout_ms must be >= 8000 (uWebSockets idleTimeout minimum is 8 s); "
                "got " + std::to_string(cfg.ping_timeout_ms));
        }

        return cfg;
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(
            std::string("Config field error in ") + path + ": " + e.what());
    }
}

} // namespace anjeer::server
