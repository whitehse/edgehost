# ARCHITECTURE.md — edgehost

## Status

**P1.5**: production io_uring HTTP/1 + `/health`, and class-A **sim_main**
fuzz path via libsim (no real sockets).

## Split (ADR-002)

| Layer | Path | Rules |
|-------|------|-------|
| **libedgecore** | `src/core/*` | Syscall-free; events; NEED_*; apply_config |
| **HTTP/1 serve** | `src/host/edge_http1_serve.c` | Shared shaggy parse + route (prod + sim) |
| **Metrics** | `src/host/edge_metrics.c` | Counters; `/health` JSON |
| **io_uring host** | `src/host/iouring_loop.c`, `main.c` | Real kernel accept/recv/send |
| **sim_main** | `src/host/sim_main.c` | Class A: libsim net/uring + same HTTP serve |
| **YAML + HUP** | `src/host/config_*.c` | libyaml; SIGHUP flag |
| **TLS (later)** | P1.13 | OpenSSL non-blocking |

## HTTP routes

| Path | Response |
|------|----------|
| `GET /health` | JSON metrics |
| `GET /` | plain `ok` |
| other GET | 404 |
| parse error | 400 |

## Class-A sim / fuzz (ADR-011)

```
edge_sim_drive(data):
  sim_fuzz_drive_a          # libsim clock/timer/net/uring opcodes
  edgecore NEED_ALLOC path  # host_alloc provide
  edge_http1_serve_feed     # direct buffer path
  sim_net + sim_uring       # accept/recv/send HTTP exchange
```

Harness: `fuzz/fuzz_edgehost_a.c` (`-DBUILD_FUZZ=ON`, clang + libFuzzer).

## TLS (ADR-014)

edgehost → OpenSSL non-blocking (later); CPE → mbedTLS.

## Deliberate absences

SPA root (P1.6), state store, plugins, OpenSSL, Prometheus (P1.15).
