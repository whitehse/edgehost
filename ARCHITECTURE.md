# ARCHITECTURE.md — edgehost

## Status

**P1.4a**: plain TCP io_uring accept + fixed static response. No shaggy parse,
no TLS yet.

## Split (ADR-002)

| Layer | Path | Rules |
|-------|------|-------|
| **libedgecore** | `src/core/*` | Syscall-free; pull events; NEED_* memory; apply_config |
| **YAML + HUP** | `src/host/config_*.c` | libyaml load; SIGHUP flag → reload |
| **io_uring host** | `src/host/iouring_loop.c`, `main.c` | accept/recv/send; process binary |
| **host_alloc** | `src/host/host_alloc.c` | Process malloc for edgecore data |
| **TLS (later)** | P1.13 | **OpenSSL non-blocking** (ADR-014); not mbedTLS |
| **Plugins / SPA** | planned | Grand functions; static UI |

## TLS (ADR-014 / Key Decision 21 corrected)

| Host | Library |
|------|---------|
| edgehost | **OpenSSL** non-blocking (`SSL_set_fd` + uring POLL) |
| CPE agent | **mbedTLS** |
| pqproxy side-car | OpenSSL/kTLS (unchanged) |

P1.4a is **plain TCP only**.

## Accept loop (P1.4a)

```
listen (nonblock) → io_uring accept
  → recv any bytes
  → send fixed static response (default HTTP/1.1 200 "ok")
  → close
```

No HTTP parse (P1.4b shaggy). No TLS (P1.13).

## Config flow (ADR-005)

Shadow YAML load → `edgecore_apply_config` → CONFIG_APPLIED / REJECTED.
SIGHUP sets flag; full live rebind of listen fd is later work.

## Deliberate absences

- shaggy bridge (P1.4b), `/health` JSON (P1.4c), state store, plugins, OpenSSL.

## Related

- Program design: `~/edge-platform-program-design.md`
- Example: `config/edgehost.example.yaml`
