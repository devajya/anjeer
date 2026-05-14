-- Migration 0012: Per-lobby bot spawn-on-leave settings
ALTER TABLE lobbies ADD COLUMN spawn_bots_on_leave BOOL NOT NULL DEFAULT FALSE;
ALTER TABLE lobbies ADD COLUMN bot_spawn_difficulty VARCHAR(10) NOT NULL DEFAULT 'easy';
ALTER TABLE lobbies ADD CONSTRAINT ck_lobbies_bot_spawn_difficulty
    CHECK (bot_spawn_difficulty IN ('easy', 'medium', 'hard', 'random'));
