# ARCHITECTURE.md — edgehost

## Status

**P1.7c**: state REST + WS stream + **lab auth / RBAC** (ADR-013).

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

## Auth (P1.7c)

| Item | Detail |
|------|--------|
| Mode | `open` (default) or `lab_password` |
| Login | `POST /auth/lab-login` `{"password":"…"}` |
| Cookie | `edge_session` HMAC-SHA256 session blob |
| Me | `GET /auth/me` |
| Secrets | env `EDGEHOST_LAB_PASSWORD`, `EDGEHOST_SESSION_HMAC` |

## Next

P1.7d proxy header HMAC; P1.8 plugin ABI.
