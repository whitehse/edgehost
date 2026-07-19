# Deploying edgehost

## Files

| Path | Purpose |
|------|---------|
| `edgehost.service` | systemd unit |
| `prometheus-scrape.snippet.yml` | Prometheus scrape job sketch for `/health` |
| `../config/edgehost.example.yaml` | lab-oriented YAML (plain TCP, open auth) |

## Install (sketch)

```bash
# Build (sibling deps + pins per AGENTS.md)
cmake -B build -S .
cmake --build build
ctest --test-dir build --output-on-failure
sudo install -m 755 build/edgehost /usr/local/bin/edgehost

# System user + dirs
sudo useradd --system --no-create-home --shell /usr/sbin/nologin edgehost
sudo mkdir -p /etc/edgehost/certs /var/lib/edgehost/spa /var/lib/edgehost/packages
sudo cp config/edgehost.example.yaml /etc/edgehost/edgehost.yaml
# Point spa.root / packages.root at /var/lib/edgehost/… ; set auth.mode for prod
# Install TLS PEMs under /etc/edgehost/certs; chown root:edgehost; chmod 640 keys

sudo install -m 644 deploy/edgehost.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now edgehost
```

## Reload

SIGHUP (via `systemctl reload edgehost`) reloads YAML through the shadow
validate → apply path (ADR-005). Live rebind of listen socket may still
require a full restart depending on release; check process logs.

## Health

- Liveness / process counters: `GET /health` → JSON (see [prometheus notes](../docs/guides/prometheus.md)).
- State: `GET/PUT/DELETE /api/v1/state/{ns}/{key}` (auth per ADR-013).
- WebSocket: `GET /api/v1/stream?topics=state`.

## Side-cars

| Service | Role |
|---------|------|
| **pqproxy** | Postgres L7 proxy; edgehost scrapes `http://127.0.0.1:9108/metrics` into `net.core/pqproxy/health` when `plugins.pqproxy.enabled` |
| **Postgres** | NOTIFY → `edge_notify_apply` when `postgres.notify_enabled` |

Do not embed pqproxy or a ClickHouse client inside edgehost.
