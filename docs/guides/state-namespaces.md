# State namespaces and ingest hooks (P1.14)

## Registered namespaces

| Namespace | Default | Typical producer |
|-----------|---------|------------------|
| `net.core` | **enabled** | metrics ingest, pqproxy health, IPFIX rollups |
| `map.dynamic` | **enabled** | Postgres NOTIFY, ops overlays |
| `net.pon` | disabled | OLT/PON status producers |
| `net.home` | disabled | CPE / home-gateway writers (key prefix often `{router_id}/…`) |
| `electric` | disabled | co-op electric telemetry |
| `inventory` | disabled | asset inventory services |
| `ext.*` | not pre-registered | third-party extensions via `edge_state_ns_set_enabled` |

Disabled namespaces return `NS_DISABLED` (HTTP 403) on put/get until enabled.

## Config (YAML)

Underscore keys (libyaml knowledge paths split on `.`):

```yaml
state:
  namespaces:
    net_core:    { enabled: true }
    map_dynamic: { enabled: true }
    net_pon:     { enabled: false }
    net_home:    { enabled: false }
    electric:    { enabled: false }
    inventory:   { enabled: false }
```

Applied at store attach via `edge_state_apply_config` (startup / io_uring
loop). Enable a hook only when a real producer is ready — empty enabled
namespaces still accept puts.

## Ingest hooks (when producers exist)

edgehost does **not** invent background pollers for disabled domains. Producers
write through existing boundaries:

| Path | Use |
|------|-----|
| **REST** `PUT /api/v1/state/{ns}/{key}` | External ingest writers; body = UTF-8 JSON |
| **NOTIFY** `edge_notify_apply` | Postgres payload schema → put/delete + WS |
| **Host API** `state_put` (plugins) | In-process plugins (e.g. pq_sidecar → `net.core`) |
| **`edge_state_ns_set_enabled`** | Dynamic `ext.*` or late enable without full restart (process API) |

### Auth for writers

| Role | State PUT | Notes |
|------|-----------|-------|
| `ingest` | yes | intended for service tokens / mTLS writers (ADR-013) |
| `employee_admin` / `employee` | yes | SPA / ops |
| `cpe` | limited | future key allowlist under `net.home/{router_id}/…` |
| open mode | all | lab default (`auth.mode: open`) |

### Value rules (ADR-007)

- Keys: `[a-z0-9_./:-]{1,128}`
- Values: UTF-8 JSON (light validation)
- Successful put/delete → WS `STATE_CHANGED` on `/api/v1/stream`

### Example: enable PON then put

```yaml
# edgehost.yaml
state:
  namespaces:
    net_pon:
      enabled: true
```

```bash
curl -sS -X PUT "http://127.0.0.1:8080/api/v1/state/net.pon/olt/olt-1" \
  -H 'Content-Type: application/json' \
  -d '{"id":"olt-1","status":"ok","updated_at":"2026-07-18T12:00:00Z"}'
```

## Related

- [ADR-007](../decisions/007-state-store.md) — store model
- [ADR-013](../decisions/013-auth-rbac-lab-session.md) — roles
- [ADR-015](../decisions/015-pqproxy-notify.md) — pqproxy + NOTIFY producers
