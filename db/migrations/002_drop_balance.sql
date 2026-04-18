-- Migration 002: remove balance column from players
-- Balance is a game-session variable (GameSession::available_cash_), not a
-- persistent player attribute. Starting balance comes from config at session init.

ALTER TABLE players DROP COLUMN IF EXISTS balance;
