# ADR-019: ClickHouse event history + Postgres ONT status

## Status

Accepted (foundation)

## Date

2026-07-21

## Context

E7 NETCONF notifications and CPE telemetry need durable, queryable history
(by ONT, shelf, PON, event type) without bloating the eager in-memory state
store. SPA just-in-time status still needs low-latency updates (NOTIFY → WS).

## Decision

1. **ClickHouse** stores event history (`edgehost.e7_netconf_events`) with
   columnar dimensions + `payload JSON` (XML converted at ingest).
2. **edgehost** owns a pure-C **clickhouse-async** client (`clickhouse-async.h`)
   that batches **JSONEachRow** HTTP inserts (flush by rows / bytes / time).
3. **E7 path**: after successful state apply, enqueue the notification to CH.
4. **CPE proxy**: `POST /api/v1/telemetry/events` shares the same aggregator.
5. **Postgres** `edgehost.ont_status` holds current ONT oper-state; trigger
   NOTIFY uses the existing `edge_notify_apply` schema for SPA fan-out.

## Consequences

- History queries do not require expanding `net.pon` max_keys.
- Batching reduces CH insert cost under notification floods.
- Full PG LISTEN writer inside edgehost can land later without schema churn.

## Alternatives considered

| Option | Why not |
|--------|---------|
| Only state store | Eager RSS; not suitable for long event history |
| C++ clickhouse-cpp | Conflicts with pure-C host (ADR-001); optional later |
| Sync insert per event | Too expensive under E7 floods |
