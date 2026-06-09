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


def _spawn_script(cfg: AnjeerConfig, lobby_code: str, game_mode: str = "simple") -> None:
    env = {
        **os.environ,
        "ANJEER_API_KEY": cfg.api_key,
        "ANJEER_SERVER_WS_URL": cfg.ws_url,
        "ANJEER_HTTP_URL": cfg.api_url,
        "ANJEER_LOBBY_CODE": lobby_code,
        "ANJEER_GAME_MODE": game_mode,
    }
    click.echo(f"Starting script: {cfg.script}")
    subprocess.run(shlex.split(cfg.script), env=env)


def _is_interactive() -> bool:
    return sys.stdin.isatty()


def _print_spectate_url(cfg: AnjeerConfig, code: str) -> None:
    try:
        token = get_spectate_token(cfg, code)
        url = _spectate_url(cfg, token)
        click.echo(f"\n🎮 Watch your game: {url}\n")
    except Exception as e:
        click.echo(f"Warning: could not get spectate URL: {e}", err=True)


@click.group()
def main() -> None:
    """anjeer — Anjeer game CLI for terminal players."""


@main.command()
def setup() -> None:
    """First-time setup: configure API key, server URLs, and script path."""
    click.echo("Anjeer setup\n")
    api_key = click.prompt("API key (from the Anjeer web app → hamburger → API Keys)")
    api_url = click.prompt("HTTP server URL", default="http://localhost:8080")
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

    click.echo(f"{'CODE':<8} {'PLAYERS':<10} {'MIN':>4} {'MAX':>4}  {'BOTS':<10}  {'GAME_MODE':<12}")
    click.echo("-" * 54)
    for l in lobbies:
        spawn = l.get("spawn_bots_on_leave", False)
        bots_col = l.get("bot_spawn_difficulty", "easy") if spawn else "off"
        gm = l.get("game_mode", "simple")
        click.echo(
            f"{l['code']:<8} {l['player_count']:<10} {l['min_players']:>4} {l['max_players']:>4}  {bots_col:<10}  {gm:<12}"
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
        lobby = poll_lobby(cfg, code)
    except Exception as e:
        click.echo(f"Error while waiting: {e}", err=True)
        sys.exit(1)

    game_mode = lobby.get("game_mode", "simple")

    _print_spectate_url(cfg, code)
    _spawn_script(cfg, code, game_mode)


@main.command()
@click.option("--min", "min_players", default=None, type=int,
              help="Minimum players required to start. (default: 2)")
@click.option("--max", "max_players", default=None, type=int,
              help="Maximum players allowed. (default: 6)")
@click.option("--spawn-bots/--no-spawn-bots", default=None,
              help="Replace leaving players with bots mid-round.")
@click.option("--bot-difficulty",
              type=click.Choice(["easy", "medium", "hard", "random"], case_sensitive=False),
              default=None,
              help="Bot difficulty when --spawn-bots is set.")
@click.option("--game-mode",
              type=click.Choice(["simple", "intermediate", "advanced"], case_sensitive=False),
              default=None,
              help="Game mode: determines feed tier and order mechanics. (default: simple)")
@click.option("--preset", default=None, metavar="NAME",
              help="Load a saved lobby preset as defaults.")
def create(
    min_players: Optional[int],
    max_players: Optional[int],
    spawn_bots: Optional[bool],
    bot_difficulty: Optional[str],
    game_mode: Optional[str],
    preset: Optional[str],
) -> None:
    """Create an API lobby. Share the code, then run 'anjeer start <CODE>' to launch."""
    try:
        cfg = load_config()
    except (FileNotFoundError, ValueError) as e:
        click.echo(str(e), err=True)
        sys.exit(1)

    # Merge priority: explicit flags > preset > hardcoded defaults.
    p: dict = {}
    if preset:
        p = cfg.presets.get(preset, {})
        if not p:
            click.echo(f"Error: preset '{preset}' not found. Run 'anjeer preset list'.", err=True)
            sys.exit(1)

    min_players    = min_players    if min_players    is not None else p.get("min_players",        2)
    max_players    = max_players    if max_players    is not None else p.get("max_players",        6)
    spawn_bots     = spawn_bots     if spawn_bots     is not None else p.get("spawn_bots_on_leave", False)
    bot_difficulty = bot_difficulty                               or  p.get("bot_difficulty",      None)
    game_mode      = game_mode      if game_mode      is not None else p.get("game_mode",          "simple")

    if spawn_bots is None:
        if _is_interactive():
            spawn_bots = click.confirm("Replace leaving players with bots?", default=False)
        else:
            spawn_bots = False

    if spawn_bots and bot_difficulty is None:
        if _is_interactive():
            bot_difficulty = click.prompt(
                "Bot difficulty",
                type=click.Choice(["easy", "medium", "hard", "random"]),
                default="easy",
            )
        else:
            bot_difficulty = "easy"

    if not spawn_bots:
        bot_difficulty = "easy"

    try:
        lobby = create_lobby(cfg, min_players, max_players,
                             spawn_bots_on_leave=spawn_bots,
                             bot_spawn_difficulty=bot_difficulty or "easy",
                             game_mode=game_mode)
    except Exception as e:
        click.echo(f"Error: {e}", err=True)
        sys.exit(1)

    code = lobby["code"]
    spawn_note = f" · bots fill leaves ({bot_difficulty})" if spawn_bots else ""
    click.echo(f"Created lobby {code} [{game_mode}] (need {min_players} players){spawn_note}")
    click.echo(f"Share this code with players, then run:  anjeer start {code}")


@main.command()
@click.argument("code")
def start(code: str) -> None:
    """Wait for players, start the game, and launch your script. Prompts to replay after each game."""
    try:
        cfg = load_config()
    except (FileNotFoundError, ValueError) as e:
        click.echo(str(e), err=True)
        sys.exit(1)

    code = code.upper()

    try:
        info = get_lobby_by_code(cfg, code)
    except Exception as e:
        click.echo(f"Error fetching lobby: {e}", err=True)
        sys.exit(1)
    if info is None:
        click.echo(f"Error: lobby '{code}' not found.", err=True)
        sys.exit(1)

    lobby_id    = info["id"]
    min_players = info.get("min_players", 2)
    max_players = info.get("max_players", 6)
    game_mode   = info.get("game_mode", "simple")

    while True:
        current = get_lobby_by_code(cfg, code)
        if current is None:
            click.echo("Error: lobby disappeared.", err=True)
            sys.exit(1)
        count = current.get("player_count", 0)
        click.echo(f"\r[{count}/{max_players}] Waiting for players…", nl=False)
        if count >= min_players:
            click.echo()
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
    _print_spectate_url(cfg, code)
    _spawn_script(cfg, code, game_mode)

    if not _is_interactive():
        return

    while True:
        click.echo("\nGame over.")
        choice = click.prompt(
            "What next?  [r=play again, n=new lobby, q=quit]",
            type=click.Choice(["r", "n", "q"], case_sensitive=False),
            default="q",
            show_choices=False,
        )
        if choice == "q":
            break
        if choice == "n":
            click.echo("Run 'anjeer create' to set up a new lobby.")
            break
        try:
            start_lobby(cfg, lobby_id)
            click.echo("New round started!")
        except Exception as e:
            click.echo(f"Error starting new round: {e}", err=True)
            continue
        _print_spectate_url(cfg, code)
        _spawn_script(cfg, code, game_mode)


# ─── preset ──────────────────────────────────────────────────────────────────

@main.group()
def preset() -> None:
    """Manage saved lobby presets."""


@preset.command(name="save")
@click.argument("name")
@click.option("--min", "min_players", default=None, type=int,
              help="Minimum players required to start.")
@click.option("--max", "max_players", default=None, type=int,
              help="Maximum players allowed.")
@click.option("--spawn-bots/--no-spawn-bots", default=None,
              help="Replace leaving players with bots mid-round.")
@click.option("--bot-difficulty",
              type=click.Choice(["easy", "medium", "hard", "random"], case_sensitive=False),
              default=None)
@click.option("--game-mode",
              type=click.Choice(["simple", "intermediate", "advanced"], case_sensitive=False),
              default=None)
def preset_save(
    name: str,
    min_players: Optional[int],
    max_players: Optional[int],
    spawn_bots: Optional[bool],
    bot_difficulty: Optional[str],
    game_mode: Optional[str],
) -> None:
    """Save a lobby preset. Only supplied options are stored."""
    try:
        cfg = load_config()
    except (FileNotFoundError, ValueError) as e:
        click.echo(str(e), err=True)
        sys.exit(1)

    entry: dict = {}
    if min_players    is not None: entry["min_players"]         = min_players
    if max_players    is not None: entry["max_players"]         = max_players
    if spawn_bots     is not None: entry["spawn_bots_on_leave"] = spawn_bots
    if bot_difficulty is not None: entry["bot_difficulty"]      = bot_difficulty
    if game_mode      is not None: entry["game_mode"]           = game_mode

    if not entry:
        click.echo("Error: no options specified. Provide at least one flag to save.", err=True)
        sys.exit(1)

    cfg.presets[name] = entry
    save_config(cfg)
    click.echo(f"Preset '{name}' saved.")


@preset.command(name="list")
def preset_list() -> None:
    """List all saved lobby presets."""
    try:
        cfg = load_config()
    except (FileNotFoundError, ValueError) as e:
        click.echo(str(e), err=True)
        sys.exit(1)

    if not cfg.presets:
        click.echo("No presets saved. Use 'anjeer preset save <name> [options]'.")
        return

    for name, values in cfg.presets.items():
        parts = ", ".join(f"{k}={v}" for k, v in values.items())
        click.echo(f"{name}: {parts}")


@preset.command(name="delete")
@click.argument("name")
def preset_delete(name: str) -> None:
    """Delete a saved preset."""
    try:
        cfg = load_config()
    except (FileNotFoundError, ValueError) as e:
        click.echo(str(e), err=True)
        sys.exit(1)

    if name not in cfg.presets:
        click.echo(f"Error: preset '{name}' not found.", err=True)
        sys.exit(1)

    del cfg.presets[name]
    save_config(cfg)
    click.echo(f"Preset '{name}' deleted.")
