# Prometheus and metrics (P1.15)

## What edgehost exposes today

| Endpoint | Format | Contents |
|----------|--------|----------|
| `GET /health` | JSON | Liveness + process counters |
| *(none)* | Prometheus text | Deferred |

`/health` body fields (see `edge_metrics_format_health_json`):

| Field | Meaning |
|-------|---------|
| `status` | always `"ok"` when the process answers |
| `uptime_s` | seconds since metrics init |
| `accepts` | successful accept completions |
| `requests` | requests that completed headers or errored |
| `responses_2xx` / `responses_4xx` | status class counts |
| `bytes_in` / `bytes_out` | cumulative payload bytes |
| `active_conns` | open client fds |
| `rejects` | accept when connection table full, etc. |

Auth: `/health` is open (no session) so probes work without lab cookies.

## Scraping without a native `/metrics`

1. **Liveness only** — Prometheus `blackbox_exporter` HTTP module against `/health`.
2. **Counters** — `prometheus-json-exporter` (or equivalent) scraping `/health` and
   mapping JSON fields to gauges/counters. See
   [`deploy/prometheus-scrape.snippet.yml`](../../deploy/prometheus-scrape.snippet.yml).
3. **Do not** parse `/health` with a Prometheus `metrics_path` scrape expecting
   exposition format — the body is JSON, not `metric_name{labels} value`.

## Related metrics surfaces

| Source | Path | Notes |
|--------|------|-------|
| **pqproxy** side-car | `http://127.0.0.1:9108/metrics` | Native Prometheus text; edgehost may scrape into `net.core/pqproxy/health` (`plugins.pqproxy`) |
| **State store** | REST/WS | Operational data (routers, map features), not process metrics |

## Future (not P1.15)

A first-class `GET /metrics` in Prometheus text format for the same counters
as `/health` is a natural follow-on. Until then, treat `/health` + an external
JSON bridge as the supported path. Keep process metrics distinct from
application state in `net.core` / `map.dynamic`.
