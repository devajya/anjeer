#include "server/config.h"

#include <fstream>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>

namespace anjeer {
namespace server {

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
        const auto& srv = j.at("server");
        ServerConfig cfg;
        cfg.host                  = srv.at("host").get<std::string>();
        cfg.port                  = srv.at("port").get<int>();
        cfg.heartbeat_interval_ms = srv.at("heartbeat_interval_ms").get<int>();
        cfg.ping_interval_ms      = srv.at("ping_interval_ms").get<int>();
        cfg.ping_timeout_ms       = srv.at("ping_timeout_ms").get<int>();
        return cfg;
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(
            std::string("Config field error in ") + path + ": " + e.what());
    }
}

} // namespace server
} // namespace anjeer
