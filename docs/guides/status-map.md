# Status map (company SPA + libwebmap demo)

First vertical for **login → WebGPU status map** on edgehost, reusing the
libwebmap demo host and rural fiber packages.

## Quick start

```bash
cd ~/edgehost
./scripts/run-status-map.sh
```

**Map + Calix E7 Call Home** (same map, plus configure/monitor SPA and
Call Home listener — default **0.0.0.0:4334**):

```bash
./scripts/run-status-map-e7.sh
# E7_HOST=0.0.0.0 E7_PORT=4334 ./scripts/run-status-map-e7.sh
# config: config/edgehost.status-map-e7.yaml → var/…runtime.yaml
# E7 guide: docs/guides/e7-callhome.md
```

**Map + Juniper Junos Call Home** (DEVICE-CONN-INFO outbound-ssh):

```bash
./scripts/run-status-map-junos.sh
# CH_HOST=0.0.0.0 CH_PORT=4334 ./scripts/run-status-map-junos.sh
# JUNOS_DEVICE_ID=pe1.lab JUNOS_SECRET=shared ./scripts/run-status-map-junos.sh
# config: config/edgehost.status-map-junos.yaml · allowlist: var/junos_allowlist.txt
```
Browser (WebGPU required — Chrome/Edge recommended):

1. Open `http://127.0.0.1:18080/`
2. Log in with password `lab` (env `EDGEHOST_LAB_PASSWORD`)
3. You are redirected to `/map/`

Also:

| URL | Purpose |
|-----|---------|
| `/map/` | Status map (session required in UI) |
| `/junos/` | Junos Call Home systems (DEVICE-ID, optional secret) |
| `/e7/` | Full Call Home admin (Calix + multi-vendor) |
| `/lab/` | Phase-1 API console (health, state, WS, packages) |
| `/health` | Process metrics JSON (no auth) |

## What the script does

1. **Symlinks** from `~/libwebmap/demo` into `spa/map/`:
   - `main.js`, `display/`, `webmap.wasm`
   - `basemap/`, `fiber_data/`, optional `weather/`, `dynamic/`, `splice_diagrams/`
2. Starts edgehost with `config/edgehost.status-map.yaml`
3. Sets lab auth env vars (`EDGEHOST_LAB_PASSWORD`, `EDGEHOST_SESSION_HMAC`)

Override demo path:

```bash
LIBWEBMAP_DEMO=/path/to/libwebmap/demo ./scripts/run-status-map.sh
```

## Configuration notes

| Setting | Why |
|---------|-----|
| `spa.max_file_bytes: 33554432` | Fiber tiles and `path_index` exceed the lab 256 KiB default |
| `auth.mode: lab_password` | Cookie session (ADR-013) |
| Directory index | `/map/` resolves to `spa/map/index.html` |

Map **data** is currently served under the SPA tree (`/map/basemap/…`) so the
unchanged demo `fetch("./basemap/…")` paths work. Auth is enforced in the map
page UI (`/auth/me` / lab login) before loading `main.js`.

**Next hardening (not in this slice):**

- Serve basemap/fiber under `/packages/` with RBAC (rewrite demo base URLs)
- Stream large files without loading whole body into the HTTP send buffer
- Live `map.dynamic` via `/api/v1/stream` (demo already has `?feed=ws://…`)

## Auth flow

```
Browser                  edgehost
   |                        |
   |  GET /                 |  SPA home (login)
   |  POST /auth/lab-login  |  Set-Cookie: edge_session=…
   |  GET /map/             |  map shell + map_boot.js
   |  GET /auth/me          |  200 if cookie valid
   |  import main.js        |  WebGPU host (libwebmap demo)
   |  GET /map/basemap/…    |  .wmap tiles
   |  GET /map/fiber_data/…  |  .fmap + path_index
```

## Lab console

The original phase-1 E2E console lives at `/lab/` (still exercised by
`scripts/lab-e2e.sh` for API paths; update bookmarks if you used `/` only).
