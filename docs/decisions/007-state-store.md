# ADR-007: State Store (`net.core` + `map.dynamic`)

## Status

Accepted (P1.7a)

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
   on put/get).
3. **Keys**: `[a-z0-9_./:-]{1,128}`; values UTF-8 JSON (lightweight validation).
4. **Limits**: default 1024 keys/ns, 4096 bytes/value (config later).
5. **REST** (lab open; auth later):
   - `GET /api/v1/state/{ns}/{key}`
   - `PUT /api/v1/state/{ns}/{key}` body = JSON
   - `DELETE /api/v1/state/{ns}/{key}`
   - `GET /api/v1/state/{ns}?prefix=`
6. **Shaggy**: server request-line accepts all RFC methods (not only GET) so
   PUT/DELETE parse correctly.
7. **Conflict / etag / TTL sweeper**: deferred (optional later).

## Consequences

- SPA and ingest tools can exercise state without plugins.
- P1.7b can emit `STATE_CHANGED` / WS from the same store mutations.
- P1.7c layers RBAC without changing key/value layout.

## Alternatives considered

| Option | Why not |
|--------|---------|
| SQLite/Postgres for v1 state | Heavier; ops Postgres remains side-car |
| Only GET until auth | Blocks vertical integration of ingest |
