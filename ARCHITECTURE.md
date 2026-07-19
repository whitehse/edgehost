# ARCHITECTURE.md — edgehost

## Status

**P1.7d**: state REST + WS stream + lab session + **proxy header HMAC** (ADR-013).

## Layers

| Layer | Path |
|-------|------|
| **edgecore** | events, config, buffers, state_store, **auth_rbac** |
| **HTTP serve** | health, SPA, packages, state, stream upgrade, **lab-login** |
| **WS hub** | host-side subscriber queue + STATE_CHANGED JSON |
| **io_uring** | production sockets; keep-alive WS after 101 |
| **sim_main** | class-A fuzz (HTTP path) |

## State REST

| Method | Path | Auth (lab_password) |
|--------|------|---------------------|
| GET/PUT/DELETE | `/api/v1/state/{ns}/{key}` | employee session |
| GET | `/api/v1/state/{ns}?prefix=` | employee session |

## WebSocket

| Item | Detail |
|------|--------|
| Path | `GET /api/v1/stream` |
| Events | `STATE_CHANGED` JSON text frames |
| Auth | employee session when `auth.mode=lab_password` |

## Auth (P1.7c–d)

| Item | Detail |
|------|--------|
| Mode | `open` (default), `lab_password`, `proxy_headers` |
| Lab login | `POST /auth/lab-login` → `edge_session` cookie |
| Proxy | `X-User` + `X-Auth-Timestamp` + `X-Auth-Signature` (+ optional `X-Roles`) |
| Me | `GET /auth/me` |
| Secrets | `EDGEHOST_LAB_PASSWORD`, `EDGEHOST_SESSION_HMAC`, `EDGEHOST_PROXY_HMAC` |

## Next

P1.8 plugin ABI.
