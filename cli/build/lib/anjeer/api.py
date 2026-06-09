import time
from typing import Optional

import requests

from .config import AnjeerConfig


def _headers(cfg: AnjeerConfig) -> dict:
    return {"Authorization": f"Bearer {cfg.api_key}"}


def find_lobbies(cfg: AnjeerConfig) -> list[dict]:
    r = requests.get(
        f"{cfg.api_url}/lobbies",
        params={"mode": "api"},
        headers=_headers(cfg),
        timeout=10,
    )
    r.raise_for_status()
    return r.json().get("lobbies", [])


def get_lobby_by_code(cfg: AnjeerConfig, code: str) -> Optional[dict]:
    r = requests.get(f"{cfg.api_url}/lobbies/{code}", timeout=10)
    if r.status_code == 404:
        return None
    r.raise_for_status()
    return r.json()


def join_lobby(cfg: AnjeerConfig, code: str) -> dict:
    r = requests.post(
        f"{cfg.api_url}/lobbies/{code}/join",
        headers=_headers(cfg),
        timeout=10,
    )
    if r.status_code == 404:
        raise ValueError(f"Lobby '{code}' not found.")
    if not r.ok:
        raise RuntimeError(f"Join failed: {r.json().get('error', r.text)}")
    body = r.json()
    return {"lobby_id": body.get("lobby_id", code), "code": body.get("code", code)}


def create_lobby(
    cfg: AnjeerConfig,
    min_players: int,
    max_players: int,
    spawn_bots_on_leave: bool = False,
    bot_spawn_difficulty: str = "easy",
    game_mode: str = "simple",
) -> dict:
    r = requests.post(
        f"{cfg.api_url}/lobbies",
        json={
            "mode": "api",
            "min_players": min_players,
            "max_players": max_players,
            "spawn_bots_on_leave": spawn_bots_on_leave,
            "bot_spawn_difficulty": bot_spawn_difficulty,
            "game_mode": game_mode,
        },
        headers=_headers(cfg),
        timeout=10,
    )
    if not r.ok:
        raise RuntimeError(f"Create failed: {r.json().get('error', r.text)}")
    return r.json()


def start_lobby(cfg: AnjeerConfig, lobby_id: str) -> None:
    r = requests.post(
        f"{cfg.api_url}/lobbies/{lobby_id}/start",
        headers=_headers(cfg),
        timeout=10,
    )
    if not r.ok:
        raise RuntimeError(f"Start failed: {r.json().get('error', r.text)}")


def poll_lobby(cfg: AnjeerConfig, code: str, interval_s: float = 2.0, timeout_s: float = 300.0) -> dict:
    deadline = time.monotonic() + timeout_s
    while True:
        lobby = get_lobby_by_code(cfg, code)
        if lobby is None:
            raise RuntimeError(f"Lobby '{code}' disappeared while waiting.")
        if lobby.get("status") == "in_game":
            return lobby
        if time.monotonic() >= deadline:
            raise TimeoutError(f"Timed out waiting for lobby '{code}' to start after {timeout_s:.0f}s")
        time.sleep(interval_s)


def leave_lobby(cfg: AnjeerConfig, code: str) -> None:
    lobby = get_lobby_by_code(cfg, code)
    if lobby is None:
        raise ValueError(f"Lobby '{code}' not found.")
    lobby_id = lobby["id"]
    r = requests.post(
        f"{cfg.api_url}/lobbies/{lobby_id}/leave",
        headers=_headers(cfg),
        timeout=10,
    )
    if r.status_code == 404:
        raise ValueError(f"Lobby '{code}' not found.")
    if not r.ok:
        raise RuntimeError(f"Leave failed: {r.json().get('error', r.text)}")


def get_spectate_token(cfg: AnjeerConfig, lobby_code: str) -> str:
    r = requests.post(
        f"{cfg.api_url}/players/me/spectate-token",
        json={"lobby_code": lobby_code},
        headers=_headers(cfg),
        timeout=10,
    )
    if not r.ok:
        raise RuntimeError(f"Spectate token error: {r.json().get('error', r.text)}")
    return r.json()["token"]
