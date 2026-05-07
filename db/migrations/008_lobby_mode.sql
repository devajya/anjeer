ALTER TABLE lobbies
    ADD COLUMN mode VARCHAR(10) NOT NULL DEFAULT 'ui';

ALTER TABLE lobbies
    ADD CONSTRAINT chk_lobby_mode CHECK (mode IN ('ui', 'api'));
