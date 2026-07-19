# State namespaces and ingest hooks (P1.14 / K10)

## Registered namespaces

| Namespace | Default | Typical producer |
|-----------|---------|------------------|
| `net.core` | **enabled** | metrics ingest, pqproxy health, IPFIX rollups |
| `map.dynamic` | **enabled** | Postgres NOTIFY, ops overlays |
| `net.pon` | disabled | OLT/PON status producers (E7 Call Home) |
| `net.home` | disabled | CPE / home-gateway writers (key prefix often `{router_id}/…`) |
| `electric` | disabled | co-op electric telemetry |
| `inventory` | disabled | asset inventory services (E7 shelf by MAC) |
| `ext.*` | not pre-registered | third-party extensions via `edge_state_ns_set_enabled` |

Disabled namespaces return `NS_DISABLED` (HTTP 403) on put/get until enabled.
They are **name stubs only** — no entry table / value buffers (K10 / PR-2a).
Enabling allocates the table eagerly; disabling frees it (keys discarded).

## Capacity (K10 / ADR-007)

Tables allocate at **enable** (or create for default-enabled ns), never on put.
Per-namespace `max_keys`; store-wide `max_value_bytes`.

```yaml
state:
  max_keys_default: 1024
  max_value_bytes: 2048          # compact; helps EDGE_WS_MSG_MAX fit
  namespaces:
    net_core:    { enabled: true }
    map_dynamic: { enabled: true }
    net_pon:     { enabled: true,  max_keys: 16384 }
    inventory:   { enabled: true,  max_keys: 512 }
    net_home:    { enabled: false }
    electric:    { enabled: false }
```

| API | Role |
|-----|------|
| `edge_state_config_from_edge_config` | `max_keys_default` / `max_value_bytes` → create |
| `edge_state_ns_set_capacity` | per-ns max_keys before/at enable |
| `edge_state_apply_config` | applies max_keys then enable flags |
| `edge_state_ns_capacity` | allocated slots (0 if stub / disabled) |

RSS example (e7 lab, eager per-ns, enable-time only): `net.pon` 16384×2048 ≈ 32 MiB;
`inventory` 512×2048 ≈ 1 MiB; `net.core`+`map.dynamic` 1024×2048×2 ≈ 4 MiB;
disabled ns ≈ 0. **Do not** raise a global 32k for all six namespaces.

## Config (YAML)

Underscore keys (libyaml knowledge paths split on `.`):

```yaml
state:
  max_keys_default: 1024
  max_value_bytes: 4096
  namespaces:
    net_core:    { enabled: true }
    map_dynamic: { enabled: true }
    net_pon:     { enabled: false }
    net_home:    { enabled: false }
    electric:    { enabled: false }
    inventory:   { enabled: false }
```

Applied at store attach via `edge_state_create_with_config` +
`edge_state_apply_config` (startup / io_uring loop). Enable a hook only when a
real producer is ready.

## Ingest hooks (when producers exist)

edgehost does **not** invent background pollers for disabled domains. Producers
write through existing boundaries:

| Path | Use |
|------|-----|
| **REST** `PUT /api/v1/state/{ns}/{key}` | External ingest writers; body = UTF-8 JSON |
| **NOTIFY** `edge_notify_apply` | Postgres payload schema → put/delete + WS |
| **Host API** `state_put` (plugins) | In-process plugins (e.g. pq_sidecar → `net.core`) |
| **`edge_state_put_and_notify`** | Unified put + `STATE_CHANGED` (PR-1 / K6) |
| **`edge_state_ns_set_enabled`** | Dynamic `ext.*` or late enable without full restart |

### Auth for writers

| Role | State PUT | Notes |
|------|-----------|-------|
| `ingest` | yes | intended for service tokens / mTLS writers (ADR-013) |
| `employee_admin` / `employee` | yes | SPA / ops |
| `cpe` | limited | future key allowlist under `net.home/{router_id}/…` |
| open mode | all | lab default (`auth.mode: open`) |

### Value rules (ADR-007)

- Keys: `[a-z0-9_./:-]{1,128}`
- Values: UTF-8 JSON (light validation); size ≤ `max_value_bytes`
- Successful put/delete → WS `STATE_CHANGED` on `/api/v1/stream`
- Full namespace → `NS_FULL` (HTTP); no silent grow

### Example: enable PON then put

```yaml
# edgehost.yaml
state:
  max_value_bytes: 2048
  namespaces:
    net_pon:
      enabled: true
      max_keys: 16384
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
- [ADR-016](../decisions/016-extra-ns-deploy.md) — extra ns enable path
