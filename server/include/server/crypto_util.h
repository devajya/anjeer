#pragma once

#include <string>
#include <string_view>

namespace anjeer::server {

// Returns lowercase hex-encoded SHA-256 digest of input. OpenSSL EVP-backed.
// ARCHITECTURE-NOTE: Called from the uWS upgrade handler (event-loop thread)
// for API key validation. The call is synchronous and blocks the event loop for
// ~1 µs on local hardware — acceptable for connection establishment. Flagged
// for async migration in Slice 16 alongside Redis infra.
std::string sha256_hex(std::string_view input);

// Returns "ank_" + 64 lowercase hex chars (32 random bytes via RAND_bytes).
// Callers store only sha256_hex(key) in the DB — the plaintext is shown once.
std::string generate_api_key();

// Returns "rtk_" + 64 lowercase hex chars (32 random bytes via RAND_bytes).
// Same storage convention as generate_api_key: store sha256_hex(token) only.
std::string generate_reconnect_token();

} // namespace anjeer::server
