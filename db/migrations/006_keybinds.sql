-- Migration 006: player_keybinds table
-- Stores per-player keyboard shortcut overrides.
-- action is a stable string key (e.g. "suit_clubs"); key_combo is the
-- serialized key string (e.g. "c", "Control+z", "ArrowUp").
-- The frontend owns the action vocabulary and default values; the server
-- is a pass-through store with no knowledge of valid actions or combos.

CREATE TABLE player_keybinds (
  player_id  BIGINT       NOT NULL REFERENCES players(id) ON DELETE CASCADE,
  action     TEXT         NOT NULL,
  key_combo  TEXT         NOT NULL,
  updated_at TIMESTAMPTZ  NOT NULL DEFAULT now(),
  PRIMARY KEY (player_id, action)
);
