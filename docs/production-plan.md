# Production Plan

## Architecture

Single EC2 instance (`t2.micro`) + RDS PostgreSQL 15 (`db.t3.micro`).

```
Internet → nginx (80/443) → Crow HTTP :10000  (REST, /health, /auth)
                          → uWS WS    :9001   (WebSocket game traffic)
                          → static    /opt/anjeer/dist  (SPA)
```

nginx terminates TLS via Let's Encrypt and rate-limits `/auth` (60 req/min per IP).

## Deployment Model

Triggered by push to `main` via `.github/workflows/deploy.yml`:
1. Build server binary + `npm run build` in CI runner
2. Bundle into tarball (`anjeer_server`, `config/prod.json`, `db/migrations/`, `dist/`)
3. `scp` tarball to EC2 via `EC2_SSH_KEY` secret
4. On EC2: run SQL migrations idempotently, swap binary, `systemctl restart anjeer`, smoke `GET /health`

Secrets never leave GitHub Actions — all 9 sensitive fields in `prod.json` are `OVERRIDE_VIA_*` sentinels replaced at runtime from `/etc/anjeer/env` (mode 600, owned by root).

## Shutdown / Restart

- SIGTERM → `ws_server.cpp` SIGTERM handler → `loop->defer(app.close())` → uWS exits cleanly
- HTTP thread watcher: if Crow exits unexpectedly (crash), `std::exit(1)` triggers `systemd` `Restart=always` with 5 s back-off
- `StartLimitBurst=5` / `StartLimitIntervalSec=60` prevents restart storms

## Observability

- All log lines go to stdout → systemd journal → CloudWatch Logs `/anjeer/syslog`
- CloudWatch custom metrics: CPU, memory, disk, network at 60 s granularity under namespace `Anjeer`
- `GET /health` (no auth) returns `{"status":"ok"}` / 503 — used by deploy smoke test and can be wired to Route 53 health checks

## Known Gaps

- No horizontal scaling: single instance, no load balancer. Sufficient for current traffic; Redis EventBus + ALB needed to scale out.
- No TLS between nginx and Crow/uWS (loopback only — acceptable on single host).
- Migrations run inline during deploy; long-running migrations will cause downtime. Use zero-downtime migration patterns (expand/contract) for future schema changes.
- Log rotation: systemd journal handles it; CloudWatch agent tails syslog. No explicit logrotate config needed.

## Runbook

**Check server status:**
```bash
systemctl status anjeer
curl http://localhost:10000/health
```

**View logs:**
```bash
journalctl -u anjeer -f
```

**Manual restart:**
```bash
systemctl restart anjeer
```

**Update secrets:**
Edit `/etc/anjeer/env`, then `systemctl restart anjeer`.

**Rollback:**
Re-run the deploy workflow from a previous commit SHA, or SSH and swap the binary manually from a prior tarball.
