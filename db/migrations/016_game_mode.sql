ALTER TABLE lobbies DROP COLUMN IF EXISTS wipe_on_trade;
ALTER TABLE lobbies ADD COLUMN IF NOT EXISTS game_mode VARCHAR(20) NOT NULL DEFAULT 'simple'
    CHECK (game_mode IN ('simple', 'intermediate', 'advanced'));

ALTER TABLE players DROP COLUMN IF EXISTS feed_preference;
