-- Migration 003: lobbies + lobby_players tables

CREATE TABLE lobbies (
    id          UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
    code        CHAR(6)      NOT NULL UNIQUE,
    owner_id    BIGINT       NOT NULL REFERENCES players(id),
    status      VARCHAR(16)  NOT NULL DEFAULT 'waiting'
                             CHECK (status IN ('waiting','starting','in_game','finished','closed')),
    min_players INT          NOT NULL DEFAULT 2,
    max_players INT          NOT NULL DEFAULT 8,
    config_json JSONB        NOT NULL DEFAULT '{}',
    created_at  TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);

CREATE TABLE lobby_players (
    lobby_id    UUID         NOT NULL REFERENCES lobbies(id) ON DELETE CASCADE,
    player_id   BIGINT       NOT NULL REFERENCES players(id),
    joined_at   TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    PRIMARY KEY (lobby_id, player_id)
);
