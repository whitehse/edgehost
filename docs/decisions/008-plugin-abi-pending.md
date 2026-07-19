# ADR-008: Plugin kinds + PENDING HTTP + host API v0 (P1.8a)

## Status

Accepted (P1.8a)

## Date

2026-07-18

## Context

edgehost must host multiple grand functions (OpenAI proxy, Slack, Teams, …)
without giving plugins raw sockets. Program design freezes two plugin kinds and
a **Pending HTTP** model for async upstream.

## Decision

1. **Kinds** (`edge_plugin.h`):
   - `EDGE_PLUGIN_KIND_HTTP` — `on_http` / `on_http_complete`
   - `EDGE_PLUGIN_KIND_SESSION` — `feed` / `next_event` (stub surface in v0)
2. **Status**: `OK` | `PENDING` | `ERR`. Only HTTP may return `PENDING`.
3. **Host API v0** (`edge_host_api_t`): `state_get` / `state_put`,
   `emit_ws_broadcast` (no-op stub), `log`, `http_client_request`,
   `schedule_timer` / `cancel_timer` (stub −1 until timer loop lands).
4. **pending_table** (`edge_pending.h`): fixed array sized
   `http.max_pending_outbound` (default 256). Keyed by `inbound_slot` and
   `outbound_id`. Create-time allocation only.
5. **Rules**:
   - At most one outstanding outbound per inbound slot (nested PENDING → 500)
   - `on_http_complete` must not return PENDING (host coerces 500)
   - PENDING without `http_client_request` → 500
   - Table full → `http_client_request` fails; plugin maps to 429
   - Cancel inbound frees slot without calling `on_http_complete`
6. **Plugins are statically linked** (dynamic `.so` later ADR).
7. **Real outbound TLS/DNS** is **P1.8b + P1.13b**; P1.8a provides ABI + table
   + test inject via `edge_plugin_host_complete_outbound`.

## Consequences

- openai_proxy (P1.8b) can implement validate → PENDING → complete without
  inventing async glue.
- Unit tests cover table semantics without io_uring or OpenSSL.
- Host uring loop will park conns when dispatch returns PENDING (wire-up with
  OpenAI routes in P1.8b).

## Alternatives considered

| Option | Why not |
|--------|---------|
| SESSION kind for OpenAI | Wrong lifecycle; request/response is HTTP PENDING |
| Host-only reverse proxy | Plugin still owns validate/normalize/audit |
| Callbacks from core | Sibling lineage is pull-event / host API only |
