# ARCHITECTURE.md — edgehost

## Status

**P1.7b**: state store REST + WebSocket `/api/v1/stream` STATE_CHANGED fan-out.

## Layers

| Layer | Path |
|-------|------|
| **edgecore** | events, config apply, buffers, **state_store** |
| **HTTP serve** | health, SPA, packages, **`/api/v1/state/…`**, stream upgrade |
| **WS hub** | host-side subscriber queue + STATE_CHANGED JSON (`edge_ws.h`) |
| **io_uring** | production sockets; keep-alive WS after 101 |
| **sim_main** | class-A fuzz (HTTP path) |

## State REST (lab open)

| Method | Path |
|--------|------|
| GET | `/api/v1/state/{ns}/{key}` |
| PUT | `/api/v1/state/{ns}/{key}` JSON body |
| DELETE | `/api/v1/state/{ns}/{key}` |
| GET | `/api/v1/state/{ns}?prefix=` |

Enabled ns: `net.core`, `map.dynamic`. Disabled stubs: `net.pon`, etc.

## WebSocket stream (P1.7b)

| Item | Detail |
|------|--------|
| Path | `GET /api/v1/stream` (optional `?topics=state`) |
| Handshake | RFC 6455 Upgrade → `101` + `Sec-WebSocket-Accept` |
| Framing | shaggy `websocket_*` (server role) |
| Events | text JSON `STATE_CHANGED` on successful PUT/DELETE |
| Auth | open lab until P1.7c |

## Next

P1.7c auth_rbac + lab session; P1.7d proxy HMAC.
