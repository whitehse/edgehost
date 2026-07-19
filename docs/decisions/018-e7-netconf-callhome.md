# ADR-018: E7 NETCONF Call Home (lab foundation)

## Status

Accepted (PR-1–PR-6 foundation; PR-7 docs). Production SSH Call Home is
deferred (PR-8 / libnetconf libassh milestone).

## Date

2026-07-19

## Context

Calix E7 OLT shelves initiate **Call Home** (RFC 8071): TCP to a dedicated
listener on edgehost, then NETCONF management. NAT and field ops require
device-initiated connections; `net.pon` / `inventory` need an in-process
producer. Full design: [docs/designs/e7-netconf-callhome.md](../designs/e7-netconf-callhome.md).

Constraints:

- Host owns multi-accept + identity (ADR-002); libnetconf owns NETCONF SM.
- ADR-007: no put-path malloc; per-ns capacity / enable-time tables (K10).
- WS hub has tight pending/message limits — ONT storms need coalescing (K16).
- libassh in libnetconf is incomplete; production SSH cannot gate lab.

## Decision

### K13 — `NETCONF_ROLE_CLIENT` after accept

1. Host `accept()` + Calix **identity preamble** parse.
2. Create `netconf_create_with_config(NETCONF_ROLE_CLIENT, &e7_profile)`.
3. NMS sends **client-shaped** `<hello>` (no `<session-id>`).
4. Do **not** use `NETCONF_ROLE_CALLHOME_SERVER` for raw NMS sessions — that
   role is the library’s NETCONF-server peer.

### K17 — MAC from identity preamble is primary shelf key

1. Pre-SSH (and pre-NETCONF) Calix identity XML supplies `<mac>` (required).
2. Normalize to lowercase colon form; allowlist / session table / state paths
   key by MAC (path segment uses hyphen form, e.g. `e7/00-02-5d-…/session`).
3. Capture serial, model, source-ip as attributes when present.
4. YAML/REST allowlist entries require `mac`; optional `shelf_id`/`label`.

### Transport: raw lab vs SSH production (K7)

| Mode | Use | Port (default) |
|------|-----|----------------|
| `transport: raw` | Lab / CI / fixtures — delimiter `]]>]]>` framing | **4334** |
| `transport: ssh` | Production Call Home after libassh (PR-8) | same path; identity **before** SSH |

- `lab_insecure_raw: true` **required** if `transport=raw` and `listen_host`
  is not loopback. Never ship `0.0.0.0` + raw without that flag and a lab
  banner.
- Production: no raw on reachable interfaces; allowlist + SSH.

### State + notify (K6 / K16)

1. Session/config/command JSON and lab.v1 ONT/PON apply go through
   `edge_state_put_and_notify` (or Call Home dirty-set → single fan-out).
2. High-rate ONT/PON updates use a **dirty-set** (`dirty_cap` default 8192),
   flush ≤100 ms on tick; overflow force-notifies.
3. Enable `net.pon` + `inventory` with compact `max_value_bytes` and raised
   `net_pon` key capacity when Call Home is on (see e7-lab YAML).

### Supporting decisions (summarized)

| # | Decision |
|---|----------|
| K10 | Per-ns capacity + enable-time eager tables (PR-2a) |
| K14 | Reduced `netconf_config_t` (`event_queue_size=8`, 256 KiB caps) + `rss_budget_bytes` (default 256 MiB) |
| K15 | Allowlist by MAC; YAML seed + runtime; `reload_policy: merge` default |
| K11 | RBAC on REST; raw only with loopback or explicit `lab_insecure_raw` |

### Module placement

| Piece | Path |
|-------|------|
| Call Home engine | `src/host/e7_callhome.c`, `include/edge_e7_callhome.h` |
| lab.v1 apply | `src/host/e7_event_apply.c`, `include/edge_e7_event_apply.h` |
| Lab config | `config/edgehost.e7-lab.yaml` |
| SPA | `spa/e7/` → `/e7/` |
| REST | `/api/v1/e7/*` (status, shelves, onts, commands) |
| Fixtures | `tests/fixtures/e7/lab_v1_*.xml` |

## Consequences

- Lab vertical works without SSH: identity → CLIENT hello → subscribe →
  lab.v1 apply → coalesced `STATE_CHANGED`.
- Operators run `config/edgehost.e7-lab.yaml` (loopback:4334); guide:
  [docs/guides/e7-callhome.md](../guides/e7-callhome.md).
- **lab.v1 is not a Calix wire format** — synthetic until real notification
  samples enable `calix.e7.*` extractors.
- Runtime REST allowlist edits are **non-durable** (lost on restart unless
  YAML updated).
- Production blocked on libnetconf SSH (PR-8); identity preamble remains
  first after accept.

## Alternatives considered

| Option | Why not |
|--------|---------|
| Outbound NETCONF only | Poor NAT story; Call Home is primary |
| `NETCONF_ROLE_CALLHOME_SERVER` for raw NMS | Wrong hello shape / server peer semantics |
| Configured-id + serial as primary key | Superseded by user decision: MAC (K17) |
| Always enable all state ns / raise global keys | Eager alloc RSS; K10 per-ns instead |
| External collector → REST | Deferred; keep in-process contracts |

## References

- Design: [e7-netconf-callhome.md](../designs/e7-netconf-callhome.md)
- Guide: [e7-callhome.md](../guides/e7-callhome.md)
- Related: ADR-002 (core/host), ADR-007 (state), ADR-013 (auth), ADR-015 (notify path)
