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
| `memory.process` | `vm_rss_kb`, `vm_hwm_kb`, `vm_size_kb`, `vm_peak_kb` from `/proc/self/status` |
| `memory.host_alloc` | tagged host heap: total `bytes` / `peak_bytes` / alloc counts + `by_kind` |
| `subsystems` (optional) | estimates: `state_rss_bytes`, live `e7_rss_estimate` / `e7_sessions_open` |

`memory.host_alloc.by_kind` categories: `edgecore`, `http`, `e7`, `ws`, `state`,
`plugin`, `other`. Outstanding bytes shrink on free (size headers on every
`host_alloc` block). Sibling libraries (OpenSSL, libnetconf, shaggy, …) are
**not** inside `host_alloc` — only process VmRSS covers them.

Lab console (`/lab/`) renders this breakdown with optional baseline deltas.

## Hierarchical memory + CPU flame (lab)

| Endpoint | Auth | Contents |
|----------|------|----------|
| `GET /api/v1/debug/memory` | lab session | Process + host_alloc + **modules** (HTTP, WS, E7 shelves/sessions, state ns, edgecore) with per-item bytes |
| `GET /api/v1/debug/cpu/capabilities` | lab session | Available samplers (SIGALRM, SIGPROF, perf_event_open, bpftrace/perf CLI if installed) |
| `POST /api/v1/debug/cpu/profile?seconds=N&mode=auto` | lab session | Start in-process stack sample (default wall-clock `ITIMER_REAL`) |
| `GET /api/v1/debug/cpu/profile` | lab session | Status / sample counts |
| `GET /api/v1/debug/cpu/profile/flame` | lab session | Flame tree JSON for lab canvas |
| `GET /api/v1/debug/cpu/profile/folded` | lab session | Folded stacks for external FlameGraph tools |

**E7 module** lists fixed tables, each **live session** (RX/TX + libnetconf estimate), and each **runtime shelf** (allowlist row + attached session cost).

**CPU sampling**: in-process `host_tick` tightens the io_uring wait to ~10 ms and
calls `backtrace()`/`dladdr` from normal context (~100 Hz wall samples). No root
required. Offline eBPF/perf: `scripts/cpu-flame-perf.sh` (install `linux-perf` +
[FlameGraph](https://github.com/brendangregg/FlameGraph); may need
`perf_event_paranoid` ≤ 1). Example bpftrace (root):
`bpftrace -e 'profile:hz:99 /pid == $PID/ { @[ustack] = count(); }'`.

Auth: `/health` is open (no session) so probes work without lab cookies.
Debug APIs require a lab session when auth is enforced.

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
