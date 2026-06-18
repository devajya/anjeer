# CLI & Scripted Players

A Python CLI for managing lobbies and launching scripted players in API-mode games.

## Installation

```bash
pip install -e cli/          # RECOMMENDED: dev install from repo root
# or
uv tool install anjeer       # production install
```

Run `anjeer setup` from your bot's project directory. It prompts for your API key and preferred language, saves config to `anjeer.json` in the current directory, and copies a starter script template.

## Commands

```bash
anjeer setup                              # first-time setup in cwd
anjeer find                               # list open API-mode lobbies
anjeer create                             # interactive lobby creation
anjeer create --game-mode advanced \
              --spawn-bots \
              --bot-difficulty hard       # non-interactive
anjeer join <code>                        # join a lobby and launch your script on start
anjeer start <code>                       # (creator) start the game and launch your script
anjeer close <code>                       # leave and delete if empty
anjeer preset list/save/delete            # manage saved lobby configurations
```

`anjeer start` and `anjeer join` set environment variables (`ANJEER_API_KEY`, `ANJEER_SERVER_WS_URL`, `ANJEER_LOBBY_CODE`, `ANJEER_GAME_MODE`) then exec your configured script. Your script connects to the same WebSocket the browser uses and receives the full wire protocol.

## Scripted Players

Scripts receive everything: MBP-N depth snapshots, MBO order lifecycle events, eval module outputs, delta tables, and round lifecycle messages. This is enough data surface to build serious strategies.

The spectator view shows a live script log panel — your script can send `script_log` messages over the WebSocket and they appear in real time for anyone watching.

A few directions people might take this:

- **Bayesian agent** — replicate or extend the hard bot's hypergeometric posterior in Python with full access to your own hand
- **Time-series model** — buffer depth snapshots and trade events into a feature matrix; act on confidence crossings
- **Reinforcement learning** — treat each round as an episode; reward = end-of-round balance delta

The interface is stable enough for real strategies. Helper templates covering connection setup, book reconstruction, and hand tracking are planned.
