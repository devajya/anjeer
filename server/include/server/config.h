#pragma once

#include <string>

namespace anjeer {
namespace server {

// AGENT-CTX: All configurable values live here — never use literals for thresholds,
// ports, or intervals anywhere in application code. Tests pass a test.json override
// to the server binary via --config to avoid touching production defaults.
struct ServerConfig {
    std::string host;
    int         port;
    int         heartbeat_interval_ms;
    // AGENT-CTX: ping_interval_ms + ping_timeout_ms must sum to ≤ 3000 to satisfy
    // the "disconnection detected within 3 seconds" acceptance criterion.
    // uWebSockets uses idleTimeout (= ping_timeout_ms / 1000) to drive ping/pong.
    int         ping_interval_ms;
    int         ping_timeout_ms;
};

// Load and parse a JSON config file.
// Throws std::runtime_error if the file cannot be opened or the JSON is malformed
// or missing required fields.
// AGENT-CTX: No defaults are applied here — every field must be present in the file.
// This makes misconfiguration loudly visible rather than silently falling back.
ServerConfig load_config(const std::string& path);

} // namespace server
} // namespace anjeer
