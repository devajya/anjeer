-- Migration 001: players table
-- No email column: identity comes entirely from (oauth_provider, oauth_id).
-- No balance column: balance is a game-session variable (GameSession::available_cash_),
-- not a persistent player attribute. games_played is denormalised, stubbed at 0 until
-- Slice 7 wires it via game_sessions.

CREATE TABLE players (
  id             BIGSERIAL    PRIMARY KEY,
  username       TEXT         NOT NULL,
  oauth_provider TEXT         NOT NULL,
  oauth_id       TEXT         NOT NULL,
  games_played   INT          NOT NULL DEFAULT 0,
  created_at     TIMESTAMPTZ  NOT NULL DEFAULT now(),
  UNIQUE (oauth_provider, oauth_id),
  UNIQUE (username)
);
