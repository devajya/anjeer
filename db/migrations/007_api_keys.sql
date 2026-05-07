CREATE TABLE api_keys (
    id          SERIAL PRIMARY KEY,
    player_id   INTEGER      NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    key_hash    VARCHAR(64)  NOT NULL UNIQUE,
    name        VARCHAR(100) NOT NULL,
    created_at  TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    expires_at  TIMESTAMPTZ  NOT NULL,
    revoked_at  TIMESTAMPTZ
);

CREATE INDEX idx_api_keys_player_id ON api_keys(player_id);

-- Enforces at-most-one non-revoked key per player (application also guards expiry).
CREATE UNIQUE INDEX idx_api_keys_one_active_per_player
    ON api_keys(player_id) WHERE revoked_at IS NULL;
