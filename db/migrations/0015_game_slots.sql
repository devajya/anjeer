-- Migration 0015: game_slots
-- Tracks per-slot state within a game session for disconnected players.
-- Enables hold-and-score: payout is computed against the held hand even
-- if the player's WS is gone. Bot slots (negative player_id) are never
-- written here — only real players. All writes are async fire-and-forget
-- from the server; the FK on session_id is safe because the game_sessions
-- row is committed synchronously before any slot rows are created.

CREATE TABLE game_slots (
    id               UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
    session_id       UUID         NOT NULL REFERENCES game_sessions(id) ON DELETE CASCADE,
    player_id        BIGINT       NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    slot_index       INT          NOT NULL,
    status           VARCHAR(20)  NOT NULL DEFAULT 'active'
                                  CHECK (status IN ('active', 'disconnected', 'expired')),
    pending_payout   INT          NOT NULL DEFAULT 0,
    disconnected_at  TIMESTAMPTZ,
    created_at       TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    updated_at       TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    UNIQUE (session_id, slot_index)
);

CREATE INDEX idx_game_slots_session ON game_slots(session_id);
CREATE INDEX idx_game_slots_player  ON game_slots(player_id);
