-- Migration 005: rename lobbies.owner_id → creator_id; add game_sessions, rounds, session_errors

ALTER TABLE lobbies RENAME COLUMN owner_id TO creator_id;

CREATE TABLE game_sessions (
    id            UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    lobby_id      UUID        NOT NULL REFERENCES lobbies(id),
    status        VARCHAR(16) NOT NULL DEFAULT 'active'
                              CHECK (status IN ('active','ended','crashed')),
    rounds_played INT         NOT NULL DEFAULT 0,
    started_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    ended_at      TIMESTAMPTZ
);

CREATE TABLE rounds (
    id            UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    session_id    UUID        NOT NULL REFERENCES game_sessions(id) ON DELETE CASCADE,
    round_number  INT         NOT NULL,
    goal_suit     VARCHAR(16) NOT NULL,
    pot           INT         NOT NULL DEFAULT 0,
    started_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    ended_at      TIMESTAMPTZ
);

-- Minimal session error log; extended when full observability stack is added
CREATE TABLE session_errors (
    id            UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    session_id    UUID        NOT NULL REFERENCES game_sessions(id) ON DELETE CASCADE,
    error_type    VARCHAR(64) NOT NULL,
    message       TEXT        NOT NULL,
    context_json  JSONB       NOT NULL DEFAULT '{}',
    occurred_at   TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
