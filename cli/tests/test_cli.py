import json
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest
from click.testing import CliRunner

from anjeer.api import poll_lobby
from anjeer.cli import main
from anjeer.config import AnjeerConfig, load_config, save_config
from tests.conftest import IN_GAME_LOBBY, WAITING_LOBBY


# ─── setup / config ──────────────────────────────────────────────────────────

def test_setup_saves_config(tmp_path):
    cfg_path = tmp_path / ".anjeer" / "config.json"
    runner = CliRunner()
    with patch("anjeer.config._GLOBAL_CONFIG", cfg_path):
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


def test_setup_default_http_url_is_8080(tmp_path):
    cfg_path = tmp_path / ".anjeer" / "config.json"
    runner = CliRunner()
    with patch("anjeer.config._GLOBAL_CONFIG", cfg_path):
        result = runner.invoke(
            main,
            ["setup"],
            input="ank_testkey\n\nws://localhost:9001/ws\npython3 /tmp/bot.py\n",
        )
    assert result.exit_code == 0, result.output
    data = json.loads(cfg_path.read_text())
    assert data["api_url"] == "http://localhost:8080"


def test_save_config_uses_global_path_even_when_local_exists(tmp_path):
    local = tmp_path / "anjeer.json"
    global_cfg = tmp_path / ".anjeer" / "config.json"
    local.write_text("{}")
    cfg = AnjeerConfig(
        api_key="k",
        api_url="http://localhost:8080",
        ws_url="ws://localhost:9001/ws",
        script="python3 bot.py",
    )
    with patch("anjeer.config._GLOBAL_CONFIG", global_cfg):
        save_config(cfg)
    assert global_cfg.exists()
    assert json.loads(local.read_text()) == {}


# ─── poll_lobby timeout ───────────────────────────────────────────────────────

def test_poll_lobby_raises_on_timeout(config_file):
    cfg = AnjeerConfig(
        api_key="ank_testkey",
        api_url="http://localhost:8080",
        ws_url="ws://localhost:9001/ws",
        script="python3 /tmp/bot.py",
    )
    with patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.api.time.monotonic", side_effect=[0.0, 1.0]):
        mock_get.return_value = MagicMock(
            status_code=200, ok=True, json=lambda: WAITING_LOBBY
        )
        with pytest.raises(TimeoutError, match="Timed out"):
            poll_lobby(cfg, "APIAB1", timeout_s=0.5)


def test_join_exits_cleanly_on_timeout(config_file):
    runner = CliRunner()
    # monotonic returns 0.0 for deadline setup, then 301.0 on first check → exceeds 300s default
    with patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.requests.post") as mock_post, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.api.time.monotonic", side_effect=[0.0, 301.0]):
        mock_get.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: WAITING_LOBBY),
            MagicMock(status_code=200, ok=True, json=lambda: WAITING_LOBBY),
        ]
        mock_post.return_value = MagicMock(
            status_code=200, ok=True,
            json=lambda: {"lobby_id": "lobby-uuid-1", "code": "APIAB1"},
        )
        result = runner.invoke(main, ["join", "APIAB1"])
    assert result.exit_code == 1
    assert "Timed out" in result.output


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
    assert "GAME_MODE" in result.output
    assert "simple" in result.output


def test_find_shows_empty_when_no_lobbies(config_file):
    runner = CliRunner()
    with patch("anjeer.api.requests.get") as mock_get:
        mock_get.return_value = MagicMock(
            status_code=200, ok=True, json=lambda: {"lobbies": []}
        )
        result = runner.invoke(main, ["find"])
    assert result.exit_code == 0
    assert "No API lobbies" in result.output


# ─── feed command removed ─────────────────────────────────────────────────────

def test_feed_command_removed():
    runner = CliRunner()
    result = runner.invoke(main, ["feed", "mbo"])
    assert result.exit_code == 2
    assert "No such command" in result.output


# ─── join ────────────────────────────────────────────────────────────────────

def test_join_polls_until_in_game(config_file):
    runner = CliRunner()
    with patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.requests.post") as mock_post, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.api.time.monotonic", return_value=0.0), \
         patch("anjeer.cli.subprocess.run") as mock_sub:
        mock_get.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: WAITING_LOBBY),
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


def test_join_prints_spectate_url(config_file):
    runner = CliRunner()
    with patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.requests.post") as mock_post, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.api.time.monotonic", return_value=0.0), \
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
         patch("anjeer.api.time.monotonic", return_value=0.0), \
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
    env = mock_sub.call_args.kwargs.get("env") or mock_sub.call_args[1].get("env")
    assert env["ANJEER_API_KEY"] == "ank_testkey"
    assert env["ANJEER_SERVER_WS_URL"] == "ws://localhost:9001/ws"
    assert env["ANJEER_LOBBY_CODE"] == "APIAB1"
    assert env["ANJEER_GAME_MODE"] == "simple"


def test_join_game_mode_propagated_to_env(config_file):
    advanced_in_game = {**IN_GAME_LOBBY, "game_mode": "advanced"}
    runner = CliRunner()
    with patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.requests.post") as mock_post, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.api.time.monotonic", return_value=0.0), \
         patch("anjeer.cli.subprocess.run") as mock_sub:
        mock_get.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: {**WAITING_LOBBY, "game_mode": "advanced"}),
            MagicMock(status_code=200, ok=True, json=lambda: advanced_in_game),
        ]
        mock_post.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: {"lobby_id": "lobby-uuid-1", "code": "APIAB1"}),
            MagicMock(status_code=200, ok=True, json=lambda: {"token": "stk_abc", "lobby_code": "APIAB1"}),
        ]
        result = runner.invoke(main, ["join", "APIAB1"])
    assert result.exit_code == 0, result.output
    env = mock_sub.call_args.kwargs.get("env") or mock_sub.call_args[1].get("env")
    assert env["ANJEER_GAME_MODE"] == "advanced"


# ─── create ──────────────────────────────────────────────────────────────────

def test_create_creates_api_lobby(config_file):
    runner = CliRunner()
    with patch("anjeer.api.requests.post") as mock_post, \
         patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.api.time.monotonic", return_value=0.0), \
         patch("anjeer.cli.subprocess.run"), \
         patch("anjeer.cli.time.sleep"):
        mock_post.side_effect = [
            MagicMock(status_code=201, ok=True, json=lambda: {**WAITING_LOBBY, "player_count": 4}),
            MagicMock(status_code=200, ok=True, json=lambda: {}),
            MagicMock(status_code=200, ok=True, json=lambda: {"token": "stk_abc", "lobby_code": "APIAB1"}),
        ]
        mock_get.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: {**WAITING_LOBBY, "player_count": 4}),
            MagicMock(status_code=200, ok=True, json=lambda: IN_GAME_LOBBY),
        ]
        result = runner.invoke(main, ["create"], input="y\n")
    assert result.exit_code == 0, result.output
    create_call = mock_post.call_args_list[0]
    body = create_call.kwargs.get("json") or create_call[1].get("json")
    assert body["mode"] == "api"
    assert body["game_mode"] == "simple"


def test_create_game_mode_advanced(config_file):
    runner = CliRunner()
    with patch("anjeer.api.requests.post") as mock_post, \
         patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.api.time.monotonic", return_value=0.0), \
         patch("anjeer.cli.subprocess.run"), \
         patch("anjeer.cli.time.sleep"):
        mock_post.side_effect = [
            MagicMock(status_code=201, ok=True, json=lambda: {**WAITING_LOBBY, "game_mode": "advanced", "player_count": 4}),
            MagicMock(status_code=200, ok=True, json=lambda: {}),
            MagicMock(status_code=200, ok=True, json=lambda: {"token": "stk_x", "lobby_code": "APIAB1"}),
        ]
        mock_get.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: {**WAITING_LOBBY, "game_mode": "advanced", "player_count": 4}),
            MagicMock(status_code=200, ok=True, json=lambda: {**IN_GAME_LOBBY, "game_mode": "advanced"}),
        ]
        result = runner.invoke(main, ["create", "--game-mode", "advanced"], input="y\n")
    assert result.exit_code == 0, result.output
    body = mock_post.call_args_list[0].kwargs.get("json") or mock_post.call_args_list[0][1].get("json")
    assert body["game_mode"] == "advanced"


def test_create_shows_player_count(config_file):
    runner = CliRunner()
    with patch("anjeer.api.requests.post") as mock_post, \
         patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.api.time.monotonic", return_value=0.0), \
         patch("anjeer.cli.subprocess.run"), \
         patch("anjeer.cli.time.sleep"):
        mock_post.side_effect = [
            MagicMock(status_code=201, ok=True, json=lambda: {**WAITING_LOBBY, "player_count": 1}),
            MagicMock(status_code=200, ok=True, json=lambda: {}),
            MagicMock(status_code=200, ok=True, json=lambda: {"token": "stk_x", "lobby_code": "APIAB1"}),
        ]
        mock_get.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: {**WAITING_LOBBY, "player_count": 4}),
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
         patch("anjeer.api.time.monotonic", return_value=0.0), \
         patch("anjeer.cli.subprocess.run"), \
         patch("anjeer.cli.time.sleep"):
        mock_post.side_effect = [
            MagicMock(status_code=201, ok=True, json=lambda: {**WAITING_LOBBY}),
            MagicMock(status_code=200, ok=True, json=lambda: {}),
            MagicMock(status_code=200, ok=True, json=lambda: {"token": "stk_x", "lobby_code": "APIAB1"}),
        ]
        mock_get.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: {**WAITING_LOBBY, "player_count": 4}),
            MagicMock(status_code=200, ok=True, json=lambda: IN_GAME_LOBBY),
        ]
        result = runner.invoke(main, ["create"], input="n\n")
    assert "Start game?" in result.output
    assert result.exit_code == 0


def test_create_starts_game_on_confirm(config_file):
    runner = CliRunner()
    with patch("anjeer.api.requests.post") as mock_post, \
         patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.api.time.monotonic", return_value=0.0), \
         patch("anjeer.cli.subprocess.run") as mock_sub, \
         patch("anjeer.cli.time.sleep"):
        mock_post.side_effect = [
            MagicMock(status_code=201, ok=True, json=lambda: {**WAITING_LOBBY}),
            MagicMock(status_code=200, ok=True, json=lambda: {}),
            MagicMock(status_code=200, ok=True, json=lambda: {"token": "stk_x", "lobby_code": "APIAB1"}),
        ]
        mock_get.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: {**WAITING_LOBBY, "player_count": 4}),
            MagicMock(status_code=200, ok=True, json=lambda: IN_GAME_LOBBY),
        ]
        result = runner.invoke(main, ["create"], input="y\n")
    assert result.exit_code == 0, result.output
    assert mock_sub.called


def test_create_spawns_script_after_start(config_file):
    runner = CliRunner()
    with patch("anjeer.api.requests.post") as mock_post, \
         patch("anjeer.api.requests.get") as mock_get, \
         patch("anjeer.api.time.sleep"), \
         patch("anjeer.api.time.monotonic", return_value=0.0), \
         patch("anjeer.cli.subprocess.run") as mock_sub, \
         patch("anjeer.cli.time.sleep"):
        mock_post.side_effect = [
            MagicMock(status_code=201, ok=True, json=lambda: {**WAITING_LOBBY}),
            MagicMock(status_code=200, ok=True, json=lambda: {}),
            MagicMock(status_code=200, ok=True, json=lambda: {"token": "stk_x", "lobby_code": "APIAB1"}),
        ]
        mock_get.side_effect = [
            MagicMock(status_code=200, ok=True, json=lambda: {**WAITING_LOBBY, "player_count": 4}),
            MagicMock(status_code=200, ok=True, json=lambda: IN_GAME_LOBBY),
        ]
        result = runner.invoke(main, ["create"], input="y\n")
    assert result.exit_code == 0, result.output
    assert mock_sub.called
    env = mock_sub.call_args.kwargs.get("env") or mock_sub.call_args[1].get("env")
    assert env["ANJEER_LOBBY_CODE"] == "APIAB1"
    assert env["ANJEER_GAME_MODE"] == "simple"
