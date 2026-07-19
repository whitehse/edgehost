# ADR-016: Extra namespace config + deploy notes (P1.14–P1.15)

## Status

Accepted (P1.14 / P1.15)

## Date

2026-07-18

## Context

ADR-007 registered `net.pon`, `net.home`, `electric`, and `inventory` as
disabled hooks. Track 1 needs a config path to enable them when producers
exist, plus a deploy unit and Prometheus guidance for ops.

## Decision

### P1.14 — additional namespaces

1. Config flags (default off except v1 pair):
   `state_net_pon_enabled`, `state_net_home_enabled`,
   `state_electric_enabled`, `state_inventory_enabled`.
2. YAML: `state.namespaces.{net_pon,net_home,electric,inventory}.enabled`
   (underscore form for libyaml knowledge paths).
3. `edge_state_apply_config(store, cfg)` applies all six flags; called on
   every store attach in the io_uring loop (owned or external).
4. Ingest “hooks” are existing write paths (REST PUT, NOTIFY apply, plugin
   host API) — no invented producers. Document in
   `docs/guides/state-namespaces.md`.
5. `ext.*` remains dynamic via `edge_state_ns_set_enabled`.

### P1.15 — deploy + Prometheus

1. `deploy/edgehost.service` systemd unit (hardened, HUP reload).
2. `deploy/README.md` install sketch.
3. Metrics remain JSON at `GET /health`; Prometheus notes + scrape snippet
   (json_exporter / blackbox / pqproxy side-car). Native text `/metrics`
   deferred.

## Consequences

- Operators enable a domain namespace when wiring a real writer.
- Lab example YAML keeps extra ns disabled.
- Monitoring can probe `/health` without waiting for exposition format.

## Alternatives considered

| Option | Why not |
|--------|---------|
| Always enable all ns | Accepts junk writes before producers exist |
| Native Prometheus in P1.15 | Scope is “notes”; JSON bridge is enough for v1 |
| Separate `state_ingest` plugin binary | REST + NOTIFY + host API already cover writers |
