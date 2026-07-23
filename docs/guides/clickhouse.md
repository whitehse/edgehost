# ClickHouse + Postgres ONT status (edgehost)

## Goals

1. **History / analytics** of E7 NETCONF (and CPE) events in **ClickHouse**.
2. **Aggregated writes** (batch JSONEachRow) for efficiency.
3. **CPE telemetry proxy**: devices POST to edgehost; edgehost batches to CH.
4. **Current ONT status** in **Postgres** for SPA just-in-time updates via NOTIFY.

```text
E7 Call Home ──► e7_callhome ──► net.pon state + WS
                      │
                      └──► clickhouse-async batch ──► CH e7_netconf_events

CPE ──POST /api/v1/telemetry/events──► same batcher ──► CH

Postgres edgehost.ont_status ──NOTIFY──► edge_notify_apply ──► net.pon + WS
```

## ClickHouse schema

Apply:

```bash
clickhouse-client --multiquery < sql/clickhouse/001_e7_netconf_events.sql
```

Table `edgehost.e7_netconf_events`:

| Column | Role |
|--------|------|
| `event_time` | From notification or ingest time |
| `shelf_id` / `shelf_mac` | Shelf identity |
| `ont_id` / `pon_id` | Query dimensions |
| `event_type` / `severity` / `source` | Event class |
| `payload` | **JSON** (XML converted) for analysis |
| `xml_raw` | Original XML (ZSTD) |

Query examples:

```sql
SELECT event_time, event_type, severity, payload
FROM edgehost.e7_netconf_events
WHERE ont_id = '1/1/3/12'
ORDER BY event_time DESC
LIMIT 100;

SELECT * FROM edgehost.e7_netconf_events
WHERE shelf_id = '00:02:5d:d9:21:47' AND pon_id = '1/1/3'
  AND event_type = 'ont-event';
```

If your ClickHouse build lacks the `JSON` type, change `payload JSON` to
`payload String` and use `JSONExtract*`.

## clickhouse-async.h

edgehost ships a **pure-C** async client at
`include/clickhouse/clickhouse-async.h`:

- Queue JSON object rows
- Flush on **row count / byte size / interval**
- HTTP `INSERT … FORMAT JSONEachRow` via outbound client

This is the ClickHouse-C binding surface used by the host (no C++ dependency).

## Config

```yaml
plugins:
  clickhouse:
    enabled: true
    host: 127.0.0.1
    port: 8123
    database: edgehost
    user: default
    events_table: edgehost.e7_netconf_events
    flush_interval_ms: 1000
    flush_max_rows: 256
    flush_max_bytes: 262144
    telemetry_proxy: true
```

With E7 enabled, each applied NETCONF notification is also enqueued (XML→JSON).

## CPE telemetry proxy

| Method | Path | Body |
|--------|------|------|
| POST | `/api/v1/telemetry/events` | One JSON object, NDJSON lines, or objects in an array |
| GET | `/api/v1/telemetry/status` | Aggregator stats |

**Auth (any of):**

1. Lab employee session cookie (SPA / curl `-b cookies.txt`)
2. **Basic Auth** when `plugins.clickhouse.telemetry_user` is set
   (`Authorization: Basic …` with that username/password → `ingest` role)
3. Open mode (`auth.mode: open`) — lab only

Response `202` when rows queued.

CPE agent NDJSON (`type=cpe_perf` / `cpe_nat` / `cpe_wifi`) is **wrapped** into
`e7_netconf_events` shape: `shelf_id=router_id`, `event_type=type`,
`payload=<original object>`, `source=cpe_agent`. Full pipeline:
`netforensics/docs/guides/cpe-agent-edgehost-pipeline.md`.

```bash
# Lab session
curl -sS -b cookies.txt -X POST http://127.0.0.1:18080/api/v1/telemetry/events \
  -H 'Content-Type: application/json' \
  -d '{"event_time":"2026-07-21 12:00:00.000","shelf_id":"cpe-1","ont_id":"1","event_type":"heartbeat","payload":{"rssi":-70}}'

# CPE device Basic Auth (telemetry_user / telemetry_password)
curl -sS -u cpe_ingest:change-me \
  -H 'Content-Type: application/x-ndjson' \
  -X POST http://127.0.0.1:18080/api/v1/telemetry/events \
  --data-binary $'{"type":"cpe_perf","router_id":"cpe-1","ts":"2026-07-22T12:00:00.000Z","probe":"ping","target":"1.1.1.1","rtt_ms":10,"loss":0}\n'
```

### ClickHouse backend credentials

`plugins.clickhouse.user` / `password` are sent as `X-ClickHouse-User` /
`X-ClickHouse-Key` on each batch flush. `use_https: true` enables TLS to CH;
default is plain HTTP on port 8123.

## Postgres ONT status

Apply:

```bash
psql "$DATABASE_URL" -f sql/postgres/001_ont_status.sql
```

- Table `edgehost.ont_status` (PK `shelf_id, ont_id`)
- Trigger NOTIFY on channel `ont_status` with `edge_notify_apply` JSON
- Helper `edgehost.upsert_ont_status(...)`

Config (DSN reserved for a future LISTEN/upsert writer in-process):

```yaml
postgres:
  ont_status:
    enabled: true
    channel: ont_status
    # dsn: postgresql://…
```

History stays in ClickHouse; Postgres holds **current** oper-state for the map/SPA.

## Related

- `sql/clickhouse/001_e7_netconf_events.sql`
- `sql/postgres/001_ont_status.sql`
- ADR-015 (NOTIFY apply), ADR-018 (E7 Call Home)
