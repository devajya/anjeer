-- Prevent a player from creating multiple open lobbies.
-- A partial unique index is used so the constraint only applies to waiting
-- lobbies; once a lobby transitions (starting/in_game/finished/closed) the
-- slot is freed and the owner can open a new one.
CREATE UNIQUE INDEX lobbies_owner_waiting_unique
    ON lobbies (owner_id)
    WHERE status = 'waiting';
