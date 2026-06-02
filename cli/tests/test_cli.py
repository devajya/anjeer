"""Test stubs — all marked xfail until implementation is complete."""
import json
from pathlib import Path
from unittest.mock import MagicMock, call, patch

import pytest
from click.testing import CliRunner

from anjeer.cli import main
from anjeer.config import AnjeerConfig, load_config, save_config
from tests.conftest import IN_GAME_LOBBY, WAITING_LOBBY


# ─── setup ───────────────────────────────────────────────────────────────────

def test_setup_saves_config(tmp_path):
    cfg_path = tmp_path / ".anjeer" / "config.json"
    runner = CliRunner()
    with patch("anjeer.config.CONFIG_PATH", cfg_path):
        result = runner.invoke(
            main,
            ["setup"],
            input="ank_testkey\nhttp://localhost:8080\nws://localhost:9001/ws\npython3 /tmp/bot.py\n",
        )
    assert result.exit_code == 0, result.output
    assert cfg_path.exists()
    data = json.loads(cfg_path.read_text())
    assert data["api_key"] == "ank_testkey"
    assert data["api_url"] == "http://localhost:8080"
    assert data["ws_url"] == "ws://localhost:9001/ws"
    assert data["script"] == "python3 /tmp/bot.py"


# ─── find ────────────────────────────────────────────────────────────────────

def test_find_prints_api_lobbies(config_file):
    runner = CliRunner()
    with patch("anjeer.api.requests.get") as mock_get:
        mock_get.return_value = MagicMock(
            status_code=200,
            ok=True,
            json=lambda: {"lobbies": [WAITING_LOBBY]},
        )
        result = runner.invoke(main, ["find"])
    assert result.exit_code == 0, result.output
    assert "APIAB1" in result.output


def test_find_shows_empty_when_no_lobbies(config_file):
    runner = CliRunner()
    with patch("anjeer.api.requests.get") as mock_get:
        mock_get.return_value = MagicMock(
            status_code=200, ok=True, json=lambda: {"lobbies": []}
        )
        result = runner.invoke(main, ["find"])
    assert result.exit_code == 0
    assert "No API lobbies" in result.output


# ─── join ────────────────────────────────────────────────────────────────────

def test_join_polls_until_in_game(config_file):
    runner = CliRunner()
    responses = [
        # get_lobby_by_code (for join)
        MagicMock(status_code=200, ok=True, json=lambda: WAITING_LOBBY),
        # post join
        MagicMock(status_code=200, ok=True, json=lambda: {"lobby_id": "lobby-uuid-1", "code": "APIAB1"}),
        # poll: first still waiting, then in_game
        MagicMock(status_code=200, ok=True, json=lambda: WAITING_LOBBY),
        MagicMock(status_code=200, ok=True, json=lambda: IN_GAME_LOBBY),
        # spectate token
        MagicMock(status_code=200, ok=True, json=lambda: {"token": "stk_abc", "lobby_code": "APIAB1"}),
    ]
    with patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.requests.post") as mock_post, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.cli.subprocess.run") as mock_sub:
        mock_get.side_effect = responses[0::2][:3]  # GET calls
        mock_post.side_effect = [responses[1], responses[4]]  # POST calls
        # Re-mock to return correct sequence
        mock_get.reset_mock()
        mock_get.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: WAITING_LOBBY),  # get_lobby_by_code in join_lobby
            MagicMock(status_code=200, ok=True, json=lambda: WAITING_LOBBY),  # poll 1
            MagicMock(status_code=200, ok=True, json=lambda: IN_GAME_LOBBY),  # poll 2
        ]
        mock_post.reset_mock()
        mock_post.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: {"lobby_id": "lobby-uuid-1", "code": "APIAB1"}),
            MagicMock(status_code=200, ok=True, json=lambda: {"token": "stk_abc", "lobby_code": "APIAB1"}),
        ]
        result = runner.invoke(main, ["join", "APIAB1"])
    assert result.exit_code == 0, result.output
    assert mock_sub.called


def test_join_prints_spectate_url(config_file):
    runner = CliRunner()
    with patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.requests.post") as mock_post, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.cli.subprocess.run"):
        mock_get.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: WAITING_LOBBY),
            MagicMock(status_code=200, ok=True, json=lambda: IN_GAME_LOBBY),
        ]
        mock_post.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: {"lobby_id": "lobby-uuid-1", "code": "APIAB1"}),
            MagicMock(status_code=200, ok=True, json=lambda: {"token": "stk_abc", "lobby_code": "APIAB1"}),
        ]
        result = runner.invoke(main, ["join", "APIAB1"])
    assert result.exit_code == 0, result.output
    assert "stk_abc" in result.output
    assert "/auth/spectate" in result.output


def test_join_spawns_script_with_env_vars(config_file):
    runner = CliRunner()
    with patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.requests.post") as mock_post, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.cli.subprocess.run") as mock_sub:
        mock_get.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: WAITING_LOBBY),
            MagicMock(status_code=200, ok=True, json=lambda: IN_GAME_LOBBY),
        ]
        mock_post.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: {"lobby_id": "lobby-uuid-1", "code": "APIAB1"}),
            MagicMock(status_code=200, ok=True, json=lambda: {"token": "stk_abc", "lobby_code": "APIAB1"}),
        ]
        result = runner.invoke(main, ["join", "APIAB1"])
    assert result.exit_code == 0, result.output
    assert mock_sub.called
    env = mock_sub.call_args.kwargs.get("env") or mock_sub.call_args[1].get("env")
    assert env is not None
    assert env["ANJEER_API_KEY"] == "ank_testkey"
    assert env["ANJEER_SERVER_WS_URL"] == "ws://localhost:9001/ws"
    assert env["ANJEER_LOBBY_CODE"] == "APIAB1"


# ─── create ──────────────────────────────────────────────────────────────────

def test_create_creates_api_lobby(config_file):
    runner = CliRunner()
    with patch("anjeer.api.requests.post") as mock_post, \
         patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.cli.subprocess.run"), \
         patch("anjeer.cli.time.sleep"):
        mock_post.side_effect = [
            MagicMock(status_code=201, ok=True, json=lambda: {**WAITING_LOBBY, "player_count": 2}),  # create
            MagicMock(status_code=200, ok=True, json=lambda: {"token": "stk_abc", "lobby_code": "APIAB1"}),  # start
            MagicMock(status_code=200, ok=True, json=lambda: {"token": "stk_abc2", "lobby_code": "APIAB1"}),  # spectate
        ]
        mock_get.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: {**WAITING_LOBBY, "player_count": 2}),  # poll
            MagicMock(status_code=200, ok=True, json=lambda: IN_GAME_LOBBY),
        ]
        result = runner.invoke(main, ["create"], input="y\n")
    assert result.exit_code == 0, result.output
    # Verify the POST /lobbies call included mode=api
    create_call = mock_post.call_args_list[0]
    body = create_call.kwargs.get("json") or create_call[1].get("json")
    assert body is not None
    assert body.get("mode") == "api"


def test_create_shows_player_count(config_file):
    runner = CliRunner()
    with patch("anjeer.api.requests.post") as mock_post, \
         patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.cli.subprocess.run"), \
         patch("anjeer.cli.time.sleep"):
        mock_post.side_effect = [
            MagicMock(status_code=201, ok=True, json=lambda: {**WAITING_LOBBY, "player_count": 1}),
            MagicMock(status_code=200, ok=True, json=lambda: {}),
            MagicMock(status_code=200, ok=True, json=lambda: {"token": "stk_x", "lobby_code": "APIAB1"}),
        ]
        mock_get.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: {**WAITING_LOBBY, "player_count": 2}),
            MagicMock(status_code=200, ok=True, json=lambda: IN_GAME_LOBBY),
        ]
        result = runner.invoke(main, ["create"], input="y\n")
    assert result.exit_code == 0, result.output
    assert "APIAB1" in result.output


def test_create_prompts_when_min_players_reached(config_file):
    runner = CliRunner()
    with patch("anjeer.api.requests.post") as mock_post, \
         patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.cli.subprocess.run"), \
         patch("anjeer.cli.time.sleep"):
        mock_post.side_effect = [
            MagicMock(status_code=201, ok=True, json=lambda: {**WAITING_LOBBY}),
            MagicMock(status_code=200, ok=True, json=lambda: {}),
            MagicMock(status_code=200, ok=True, json=lambda: {"token": "stk_x", "lobby_code": "APIAB1"}),
        ]
        mock_get.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: {**WAITING_LOBBY, "player_count": 2}),
            MagicMock(status_code=200, ok=True, json=lambda: IN_GAME_LOBBY),
        ]
        # Send 'n' to cancel after prompt
        result = runner.invoke(main, ["create"], input="n\n")
    assert "Start game?" in result.output
    assert result.exit_code == 0


def test_create_starts_game_on_confirm(config_file):
    runner = CliRunner()
    with patch("anjeer.api.requests.post") as mock_post, \
         patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.cli.subprocess.run") as mock_sub, \
         patch("anjeer.cli.time.sleep"):
        mock_post.side_effect = [
            MagicMock(status_code=201, ok=True, json=lambda: {**WAITING_LOBBY}),
            MagicMock(status_code=200, ok=True, json=lambda: {}),  # start
            MagicMock(status_code=200, ok=True, json=lambda: {"token": "stk_x", "lobby_code": "APIAB1"}),
        ]
        mock_get.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: {**WAITING_LOBBY, "player_count": 2}),
            MagicMock(status_code=200, ok=True, json=lambda: IN_GAME_LOBBY),
        ]
        result = runner.invoke(main, ["create"], input="y\n")
    assert result.exit_code == 0, result.output
    assert mock_sub.called


# ─── feed ────────────────────────────────────────────────────────────────────

def test_feed_mbp1_alias_calls_put(config_file):
    """T32: anjeer feed mbp-1 → PUT /players/me/feed with body {"feed_preference":"mbp1"}"""
    runner = CliRunner()
    with patch("anjeer.api.requests.put") as mock_put:
        mock_put.return_value = MagicMock(
            ok=True, json=lambda: {"feed_preference": "mbp1"}
        )
        result = runner.invoke(main, ["feed", "mbp-1"])
    assert result.exit_code == 0, result.output
    assert "mbp1" in result.output
    body = mock_put.call_args.kwargs.get("json") or mock_put.call_args[1].get("json")
    assert body == {"feed_preference": "mbp1"}


def test_feed_mbo_calls_put(config_file):
    """T33: anjeer feed mbo → PUT /players/me/feed with body {"feed_preference":"mbo"}"""
    runner = CliRunner()
    with patch("anjeer.api.requests.put") as mock_put:
        mock_put.return_value = MagicMock(
            ok=True, json=lambda: {"feed_preference": "mbo"}
        )
        result = runner.invoke(main, ["feed", "mbo"])
    assert result.exit_code == 0, result.output
    assert "mbo" in result.output
    body = mock_put.call_args.kwargs.get("json") or mock_put.call_args[1].get("json")
    assert body == {"feed_preference": "mbo"}


def test_feed_invalid_exits_nonzero(config_file):
    """T34: anjeer feed <invalid> → exits non-zero, prints usage error"""
    runner = CliRunner()
    result = runner.invoke(main, ["feed", "turbo"])
    assert result.exit_code != 0


def test_create_spawns_script_after_start(config_file):
    runner = CliRunner()
    with patch("anjeer.api.requests.post") as mock_post, \
         patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.cli.subprocess.run") as mock_sub, \
         patch("anjeer.cli.time.sleep"):
        mock_post.side_effect = [
            MagicMock(status_code=201, ok=True, json=lambda: {**WAITING_LOBBY}),
            MagicMock(status_code=200, ok=True, json=lambda: {}),
            MagicMock(status_code=200, ok=True, json=lambda: {"token": "stk_x", "lobby_code": "APIAB1"}),
        ]
        mock_get.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: {**WAITING_LOBBY, "player_count": 2}),
            MagicMock(status_code=200, ok=True, json=lambda: IN_GAME_LOBBY),
        ]
        result = runner.invoke(main, ["create"], input="y\n")
    assert result.exit_code == 0, result.output
    assert mock_sub.called
    env = mock_sub.call_args.kwargs.get("env") or mock_sub.call_args[1].get("env")
    assert env["ANJEER_LOBBY_CODE"] == "APIAB1"
