-- Migration 0014: reconnect_tokens
-- Short-lived tokens issued per game slot on join. Stored hashed (SHA-256)
-- so they survive a server restart as a hook for future crash-recovery slices.
-- Only the plaintext is sent to the client; the server never stores it.

CREATE TABLE reconnect_tokens (
    id          UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
    token_hash  CHAR(64)     NOT NULL UNIQUE,
    player_id   BIGINT       NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    lobby_id    UUID         NOT NULL REFERENCES lobbies(id) ON DELETE CASCADE,
    session_id  UUID         REFERENCES game_sessions(id) ON DELETE SET NULL,
    expires_at  TIMESTAMPTZ  NOT NULL,
    created_at  TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_reconnect_tokens_hash         ON reconnect_tokens(token_hash);
CREATE INDEX idx_reconnect_tokens_player_lobby ON reconnect_tokens(player_id, lobby_id);
