# Lab end-to-end demo

Vertical lab slice: **edgehost** (SPA + packages + lab auth + state + WS) and
**ecoec-mobile** package pull/install.

## Prerequisites

```bash
# edgehost
cd ~/edgehost
cmake -B build -S . && cmake --build build

# ecoec-mobile (for package sync step)
cd ~/ecoec-mobile
cmake -B build -S . && cmake --build build
```

Tools: `curl`, optional `wscat` for CLI WS smoke.

## One-shot smoke

```bash
cd ~/edgehost
./scripts/lab-e2e.sh
```

What it checks:

1. `GET /health`
2. SPA `GET /`
3. `POST /auth/lab-login` → `edge_session` cookie
4. `GET /auth/me`
5. `PUT`/`GET` `/api/v1/state/net.core/router/lab-1`
6. `PUT` `map.dynamic` sample feature
7. Authenticated `GET /packages/fixture_basemap/...`
8. Unauthenticated package access denied
9. Optional WS via `wscat`
10. **mobile** download + `mobile_core_demo` tile count

Leave the server running for the browser console:

```bash
KEEP_RUNNING=1 ./scripts/lab-e2e.sh
# open http://127.0.0.1:18080/
# password: lab
```

Server-only (no mobile tree):

```bash
SKIP_MOBILE=1 ./scripts/lab-e2e.sh
```

## Manual server

```bash
export EDGEHOST_LAB_PASSWORD=lab
export EDGEHOST_SESSION_HMAC='dev-hmac-key-32-bytes-minimum!!'
cd ~/edgehost
./build/edgehost --config config/edgehost.lab.yaml --host 127.0.0.1 --port 18080
```

Browser: http://127.0.0.1:18080/ — lab console for login, state, WS, packages.

## Mobile package sync only

```bash
export EDGEHOST_LAB_PASSWORD=lab
# assume edgehost already running with lab auth

~/ecoec-mobile/scripts/sync_from_edgehost.sh \
  --base http://127.0.0.1:18080 \
  --password lab \
  --package fixture_basemap \
  --cache /tmp/ecoec-pkgs \
  --demo ~/ecoec-mobile/build/mobile_core_demo
```

Host downloads `manifest.json` + tiles listed in `tiles[]`, then
`mobile_core_demo --fixture <dir>` installs and prints `map_tile_count`.

## Optional OpenAI proxy

```bash
export OPENAI_API_KEY=sk-...
# Edit config/edgehost.lab.yaml: plugins.openai_proxy.enabled: true
# or:
ENABLE_OPENAI=1 OPENAI_API_KEY=sk-... ./scripts/lab-e2e.sh
```

Then (with session cookie):

```bash
curl -b cookies.txt -H 'Content-Type: application/json' \
  -d '{"model":"gpt-4o-mini","messages":[{"role":"user","content":"hi"}],"max_tokens":16}' \
  http://127.0.0.1:18080/v1/chat/completions
```

## Layout

| Path | Role |
|------|------|
| `config/edgehost.lab.yaml` | Lab auth + SPA + packages |
| `spa/` | Company home + lab console (`/lab/`) + status map shell (`/map/`) |
| `spa/lab/` | Phase-1 API lab console (login, state, WS, packages) |
| `packages/fixture_basemap/` | Offline basemap fixture (3 tiles) |
| `packages/index.json` | Package catalog for SPA |
| `scripts/lab-e2e.sh` | Automated smoke |
| `~/ecoec-mobile/scripts/sync_from_edgehost.sh` | Pull + install |

## Auth notes

- Lab mode: `auth.mode: lab_password` (ADR-013 step 1).
- Open: `/health`, SPA static, `POST /auth/lab-login`.
- Cookie required: `/api/v1/state/*`, `/api/v1/stream`, `/packages/*`.
- Field packages: set mobile `packages.require_signature: true` later; lab uses unsigned fixtures.
