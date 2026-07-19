# ADR-007: State Store (`net.core` + `map.dynamic`)

## Status

Accepted (P1.7a); WS fan-out in P1.7b; config enable for extra ns in P1.14
(see ADR-016).

## Date

2026-07-18

## Context

Program design Decision 4 / Key Decision 16: v1 implements two namespaces fully
for operational SPA and ingest. Auth/RBAC lands in P1.7c; WebSocket fan-out in
P1.7b.

## Decision

1. **In-memory store** (`edge_state.h` / `state_store.c`) in libedgecore —
   create-time allocation of fixed key slots; no post-create silent malloc.
2. **Namespaces**: `net.core` and `map.dynamic` enabled by default; `net.pon`,
   `net.home`, `electric`, `inventory` registered **disabled** (`NS_DISABLED`
   on put/get) until enabled via YAML / `edge_state_apply_config` (P1.14).
3. **Keys**: `[a-z0-9_./:-]{1,128}`; values UTF-8 JSON (lightweight validation).
4. **Limits**: default 1024 keys/ns, 4096 bytes/value (config later).
5. **REST** (lab open; auth later):
   - `GET /api/v1/state/{ns}/{key}`
   - `PUT /api/v1/state/{ns}/{key}` body = JSON
   - `DELETE /api/v1/state/{ns}/{key}`
   - `GET /api/v1/state/{ns}?prefix=`
6. **Shaggy**: server request-line accepts all RFC methods (not only GET) so
   PUT/DELETE parse correctly.
7. **WebSocket stream** (P1.7b, host hub):
   - `GET /api/v1/stream?topics=state` + Upgrade → `101` + shaggy framing
   - Successful PUT/DELETE → hub broadcasts `STATE_CHANGED` JSON text frames
   - Payload: `{type, ns, key, op, value, request_id}` (`value` null on delete)
   - `X-Request-Id` preserved when present; else host mints `eh…`
8. **Conflict / etag / TTL sweeper**: deferred (optional later).

## Consequences

- SPA and ingest tools can exercise state without plugins.
- Live SPA can subscribe to `STATE_CHANGED` without polling.
- P1.7c layers RBAC without changing key/value layout or WS payload shape.

## Alternatives considered

| Option | Why not |
|--------|---------|
| SQLite/Postgres for v1 state | Heavier; ops Postgres remains side-car |
| Only GET until auth | Blocks vertical integration of ingest |
