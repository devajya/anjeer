CREATE TABLE spectate_tokens (
    id          SERIAL PRIMARY KEY,
    token_hash  CHAR(64)     NOT NULL UNIQUE,
    player_id   BIGINT       NOT NULL REFERENCES players(id),
    lobby_code  VARCHAR(16)  NOT NULL,
    expires_at  TIMESTAMPTZ  NOT NULL,
    used_at     TIMESTAMPTZ
);
CREATE INDEX spectate_tokens_hash_idx ON spectate_tokens(token_hash);
