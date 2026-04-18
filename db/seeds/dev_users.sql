-- Dev seed: 5 test players for local development and manual game testing.
-- oauth_provider='github' with oauth_id='test_user_N' — these IDs are strings
-- and will never collide with real GitHub numeric IDs.
-- ON CONFLICT DO NOTHING: idempotent — `make db-seed` can be run multiple times.

INSERT INTO players (username, oauth_provider, oauth_id)
VALUES
    ('test_player_1', 'github', 'test_user_1'),
    ('test_player_2', 'github', 'test_user_2'),
    ('test_player_3', 'github', 'test_user_3'),
    ('test_player_4', 'github', 'test_user_4'),
    ('test_player_5', 'github', 'test_user_5')
ON CONFLICT (oauth_provider, oauth_id) DO NOTHING;
