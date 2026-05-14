import os
import shlex
import subprocess
import sys
import time
from typing import Optional

import click

from .api import (
    create_lobby,
    find_lobbies,
    get_lobby_by_code,
    get_spectate_token,
    join_lobby,
    poll_lobby,
    start_lobby,
)
from .config import AnjeerConfig, load_config, save_config


def _spectate_url(cfg: AnjeerConfig, token: str) -> str:
    return f"{cfg.api_url}/auth/spectate?token={token}"


def _spawn_script(cfg: AnjeerConfig, lobby_code: str) -> None:
    env = {
        **os.environ,
        "ANJEER_API_KEY": cfg.api_key,
        "ANJEER_SERVER_WS_URL": cfg.ws_url,
        "ANJEER_HTTP_URL": cfg.api_url,
        "ANJEER_LOBBY_CODE": lobby_code,
    }
    click.echo(f"Starting script: {cfg.script}")
    subprocess.run(shlex.split(cfg.script), env=env)


@click.group()
def main() -> None:
    """anjeer — Anjeer game CLI for terminal players."""


@main.command()
def setup() -> None:
    """First-time setup: configure API key, server URLs, and script path."""
    click.echo("Anjeer setup\n")
    api_key = click.prompt("API key (from the Anjeer web app → hamburger → API Keys)")
    api_url = click.prompt("HTTP server URL", default="http://localhost:10000")
    ws_url  = click.prompt("WebSocket URL",  default="ws://localhost:9001/ws")
    script  = click.prompt("Script command  (e.g. python3 /home/you/my_bot.py)")

    cfg = AnjeerConfig(
        api_key=api_key,
        api_url=api_url.rstrip("/"),
        ws_url=ws_url,
        script=script,
    )
    save_config(cfg)
    click.echo("\nConfig saved. Run 'anjeer find' to see open API lobbies.")


@main.command(name="find")
def find_cmd() -> None:
    """List waiting API-mode lobbies."""
    try:
        cfg = load_config()
    except (FileNotFoundError, ValueError) as e:
        click.echo(str(e), err=True)
        sys.exit(1)

    try:
        lobbies = find_lobbies(cfg)
    except Exception as e:
        click.echo(f"Error: {e}", err=True)
        sys.exit(1)

    if not lobbies:
        click.echo("No API lobbies found.")
        return

    click.echo(f"{'CODE':<8} {'PLAYERS':<10} {'MIN':>4} {'MAX':>4}  {'BOTS':<10}")
    click.echo("-" * 40)
    for l in lobbies:
        spawn = l.get("spawn_bots_on_leave", False)
        bots_col = l.get("bot_spawn_difficulty", "easy") if spawn else "off"
        click.echo(
            f"{l['code']:<8} {l['player_count']:<10} {l['min_players']:>4} {l['max_players']:>4}  {bots_col:<10}"
        )


@main.command()
@click.argument("code")
def join(code: str) -> None:
    """Join a lobby by code, wait for game start, then launch your script."""
    try:
        cfg = load_config()
    except (FileNotFoundError, ValueError) as e:
        click.echo(str(e), err=True)
        sys.exit(1)

    code = code.upper()

    # Show spawn-bots setting before joining so the player knows what to expect.
    try:
        info = get_lobby_by_code(cfg, code)
        if info and info.get("spawn_bots_on_leave"):
            diff = info.get("bot_spawn_difficulty", "easy")
            click.echo(f"Note: this lobby replaces leaving players with {diff} bots.")
    except Exception:
        pass

    try:
        join_lobby(cfg, code)
    except Exception as e:
        click.echo(f"Error: {e}", err=True)
        sys.exit(1)

    click.echo(f"Joined lobby {code}. Waiting for game to start…")

    try:
        poll_lobby(cfg, code)
    except Exception as e:
        click.echo(f"Error while waiting: {e}", err=True)
        sys.exit(1)

    try:
        token = get_spectate_token(cfg, code)
        url = _spectate_url(cfg, token)
        click.echo(f"\n🎮 Watch your game: {url}\n")
    except Exception as e:
        click.echo(f"Warning: could not get spectate URL: {e}", err=True)

    _spawn_script(cfg, code)


@main.command()
@click.option("--min", "min_players", default=2, show_default=True,
              help="Minimum players required to start.")
@click.option("--max", "max_players", default=6, show_default=True,
              help="Maximum players allowed.")
@click.option("--spawn-bots/--no-spawn-bots", default=None,
              help="Replace leaving players with bots mid-round.")
@click.option("--bot-difficulty",
              type=click.Choice(["easy", "medium", "hard", "random"], case_sensitive=False),
              default=None,
              help="Bot difficulty when --spawn-bots is set.")
def create(
    min_players: int,
    max_players: int,
    spawn_bots: Optional[bool],
    bot_difficulty: Optional[str],
) -> None:
    """Create an API lobby, wait for players, then start and launch your script."""
    try:
        cfg = load_config()
    except (FileNotFoundError, ValueError) as e:
        click.echo(str(e), err=True)
        sys.exit(1)

    # Interactive prompts for spawn settings when not supplied via flags.
    if spawn_bots is None:
        if sys.stdin.isatty():
            spawn_bots = click.confirm("Replace leaving players with bots?", default=False)
        else:
            spawn_bots = False

    if spawn_bots and bot_difficulty is None:
        if sys.stdin.isatty():
            bot_difficulty = click.prompt(
                "Bot difficulty",
                type=click.Choice(["easy", "medium", "hard", "random"]),
                default="easy",
            )
        else:
            bot_difficulty = "easy"

    if not spawn_bots:
        bot_difficulty = "easy"  # ignored server-side, but keep it a valid value

    try:
        lobby = create_lobby(cfg, min_players, max_players,
                             spawn_bots_on_leave=spawn_bots,
                             bot_spawn_difficulty=bot_difficulty or "easy")
    except Exception as e:
        click.echo(f"Error: {e}", err=True)
        sys.exit(1)

    code      = lobby["code"]
    lobby_id  = lobby["id"]
    spawn_note = f" · bots fill leaves ({bot_difficulty})" if spawn_bots else ""
    click.echo(f"Created lobby {code} (need {min_players} players to start){spawn_note}")

    # Poll until enough players have joined, showing a live count.
    while True:
        current = get_lobby_by_code(cfg, code)
        if current is None:
            click.echo("Error: lobby disappeared.", err=True)
            sys.exit(1)
        count = current.get("player_count", 0)
        click.echo(f"\r[{count}/{max_players}] Waiting for players…", nl=False)
        if count >= min_players:
            click.echo()  # newline after the spinner line
            break
        time.sleep(2)

    if not click.confirm("Ready! Start game?", default=True):
        click.echo("Cancelled.")
        sys.exit(0)

    try:
        start_lobby(cfg, lobby_id)
    except Exception as e:
        click.echo(f"Error starting: {e}", err=True)
        sys.exit(1)

    click.echo("Game started!")

    try:
        token = get_spectate_token(cfg, code)
        url = _spectate_url(cfg, token)
        click.echo(f"\n🎮 Watch your game: {url}\n")
    except Exception as e:
        click.echo(f"Warning: could not get spectate URL: {e}", err=True)

    _spawn_script(cfg, code)
