# ADR-015: pqproxy side-car scrape + NOTIFY state apply (P1.11–P1.12)

## Status

Accepted (P1.11 / P1.12)

## Date

2026-07-19

## Context

edgehost must not fork pqproxy. Metrics and map overlays come from side-car
HTTP scrape and Postgres NOTIFY payloads applied into the in-process state
store, then fanned out on WebSocket.

## Decision

### P1.11 — pq_sidecar

1. Config: `plugins.pqproxy.enabled`, `metrics_url`, `scrape_interval_ms`.
2. Host GETs Prometheus text from metrics_url (plain HTTP outbound).
3. Parse gauges `pqproxy_live_backends`, `pqproxy_active_frontends`,
   `pqproxy_pool_waiters`.
4. Put JSON at `net.core/pqproxy/health`:
   `{"up":true,"pool_busy":N,"live_backends":…,"active_frontends":…,"scraped_at":…}`
5. Broadcast `STATE_CHANGED` via WS hub when present.
6. Maintain tick in io_uring loop runs scrape on interval.

### P1.12 — NOTIFY apply

1. Enforced payload JSON:
   `{"ns":"…","key":"…","op":"put"|"delete","value":…,"request_id":?}`
2. `edge_notify_apply(store, hub, json, len)` validates schema, mutates state,
   broadcasts WS.
3. Max payload `EDGE_NOTIFY_MAX_PAYLOAD` (8000).
4. Full Postgres LISTEN client remains optional host work; apply API is the
   stable boundary (tests inject payloads without a live PG).

## Consequences

- SPA can show pqproxy health without linking pqproxy.
- Map dynamic features update via NOTIFY without embedding PG in libwebmap.
- Live PG LISTEN can be added without changing the payload contract.

## Alternatives considered

| Option | Why not |
|--------|---------|
| Link pqproxy into edgehost | Dilutes security surface; design is side-car only |
| Free-form NOTIFY strings | Schema enforcement required by PR plan |
