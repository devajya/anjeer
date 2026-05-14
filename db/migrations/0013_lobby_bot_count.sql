-- Migration 0013: Track in-memory bot count per lobby so HTTP start handler
-- can enforce min_players (real + bots) without accessing WsServer state.
-- Reset to 0 on lobby close; irrelevant once status != waiting.
ALTER TABLE lobbies ADD COLUMN bot_count INT NOT NULL DEFAULT 0;
