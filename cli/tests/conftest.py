import json
import pytest
from pathlib import Path
from unittest.mock import MagicMock, patch

from anjeer.config import AnjeerConfig


@pytest.fixture
def sample_config(tmp_path):
    return AnjeerConfig(
        api_key="ank_testkey",
        api_url="http://localhost:8080",
        ws_url="ws://localhost:9001/ws",
        script="python3 /tmp/bot.py",
    )


@pytest.fixture
def config_file(tmp_path, sample_config):
    """Writes config to a temp file and patches CONFIG_PATH."""
    cfg_path = tmp_path / ".anjeer" / "config.json"
    cfg_path.parent.mkdir(parents=True)
    cfg_path.write_text(json.dumps({
        "api_key": sample_config.api_key,
        "api_url": sample_config.api_url,
        "ws_url": sample_config.ws_url,
        "script": sample_config.script,
    }))
    with patch("anjeer.config.CONFIG_PATH", cfg_path):
        yield cfg_path


WAITING_LOBBY = {
    "id": "lobby-uuid-1",
    "code": "APIAB1",
    "creator_id": 1,
    "status": "waiting",
    "mode": "api",
    "min_players": 4,
    "max_players": 5,
    "player_count": 1,
    "game_mode": "simple",
}

IN_GAME_LOBBY = {**WAITING_LOBBY, "status": "in_game", "player_count": 4}
