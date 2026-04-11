# Anjeer

A multiplayer card trading game. This repo contains the C++ game server, matching engine, and React frontend.

---

## Quick start

```bash
# First time only — install frontend dependencies
npm install --prefix frontend

# Start both the C++ server and the Vite dev frontend
make dev
```

Open [http://localhost:5173](http://localhost:5173) in your browser. You should see a green **Connected** banner and a live timestamp updating every second.

---

## Prerequisites

See `contexts/machine-setup.md` for the full provisioning guide. Short version:

| Tool | Required version |
|---|---|
| g++ | 11+ |
| cmake | 3.20+ |
| Node | 18+ |
| npm | 9+ |
| libpqxx-dev | system package |
| clang-format | any recent |
| PostgreSQL | 15 (port 5433) |

---

## Config

All runtime behaviour is controlled by JSON config files. The server is started with:

```bash
./build/server/server --config config/default.json
```

### `config/default.json` (committed)

```json
{
  "server": {
    "host": "0.0.0.0",
    "port": 9001,
    "heartbeat_interval_ms": 1000,
    "ping_interval_ms": 1000,
    "ping_timeout_ms": 3000
  }
}
```

### `config/dev.json` (gitignored — create locally to override)

Copy any fields from `default.json` you want to change. Example:

```json
{
  "server": {
    "port": 9002
  }
}
```

> If you change the port in `dev.json`, also update the proxy target in `frontend/vite.config.ts`.

---

## Makefile commands

```bash
make build          # Build C++ server + frontend
make build-engine   # Build engine library only
make build-server   # Build server binary only
make build-frontend # Build frontend for production

make dev            # Start C++ server + Vite dev server (concurrent)
make dev-server     # Start C++ server only
make dev-frontend   # Start Vite dev server only

make test           # Run all tests (C++ unit + frontend)
make test-unit      # Run Catch2 tests
make test-frontend  # Run Vitest tests

make clean          # Remove build/ and frontend/node_modules
make fmt            # Format all C++ and frontend source files
```

---

## Project layout

```
anjeer/
├── engine/           # C++ matching engine and game logic (grows from Slice 2)
├── server/           # C++ WebSocket server, HTTP endpoints, DB writer
├── frontend/         # React + TypeScript + Vite client
├── tests/
│   └── integration/  # Python multi-client WebSocket test harness (from Slice 3)
├── db/
│   └── migrations/   # Sequential SQL files (from Slice 5)
├── scripts/          # migrate.sh, seed.sh (from Slice 5)
├── config/
│   ├── default.json  # Committed defaults
│   └── dev.json      # Local overrides — gitignored
└── Makefile          # All dev commands
```
