# ARCHITECTURE.md — edgehost

## Status

**P1.4c**: plain TCP io_uring + shaggy HTTP/1 + **`GET /health` JSON** and
basic process metrics counters.

## Split (ADR-002)

| Layer | Path | Rules |
|-------|------|-------|
| **libedgecore** | `src/core/*` | Syscall-free; pull events; NEED_* memory; apply_config |
| **YAML + HUP** | `src/host/config_*.c` | libyaml load; SIGHUP flag → reload |
| **io_uring + HTTP/1** | `src/host/iouring_loop.c`, `main.c` | accept/recv/send; shaggy parse |
| **Metrics** | `src/host/edge_metrics.c` | Counters; `/health` JSON |
| **host_alloc** | `src/host/host_alloc.c` | Process malloc for edgecore data |
| **TLS (later)** | P1.13 | **OpenSSL non-blocking** (ADR-014) |

## HTTP routes (P1.4c)

| Method / path | Response |
|---------------|----------|
| `GET /health` | `200 application/json` — status, uptime, accepts, requests, 2xx/4xx, bytes, active_conns, rejects |
| `GET /` | `200 text/plain` — `ok\n` |
| other `GET` | `404 text/plain` |
| parse error | `400 text/plain` |

## Metrics fields

`accepts`, `requests`, `responses_2xx`, `responses_4xx`, `bytes_in`,
`bytes_out`, `active_conns`, `rejects`, `uptime_s`.

Not Prometheus exposition yet (P1.15).

## TLS (ADR-014)

edgehost → OpenSSL non-blocking (later); CPE → mbedTLS; pqproxy side-car OpenSSL.

## Deliberate absences

- SPA root (P1.6), state store (P1.7a), plugins, OpenSSL, sim_main (P1.5).

## Related

- Program design: `~/edge-platform-program-design.md`
