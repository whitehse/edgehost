# ARCHITECTURE.md — edgehost

## Status

**P1.4b**: plain TCP io_uring accept + **shaggy HTTP/1** parse bridge.
Response body is still a simple static `ok\n` (P1.4c adds real `/health` JSON).

## Split (ADR-002)

| Layer | Path | Rules |
|-------|------|-------|
| **libedgecore** | `src/core/*` | Syscall-free; pull events; NEED_* memory; apply_config |
| **YAML + HUP** | `src/host/config_*.c` | libyaml load; SIGHUP flag → reload |
| **io_uring + HTTP/1** | `src/host/iouring_loop.c`, `main.c` | accept/recv/send; drives shaggy `http1_*` |
| **host_alloc** | `src/host/host_alloc.c` | Process malloc for edgecore data |
| **TLS (later)** | P1.13 | **OpenSSL non-blocking** (ADR-014) |
| **Plugins / SPA** | planned | Grand functions; static UI |

## HTTP/1 bridge (P1.4b)

```
accept → recv
  → http1_create(HTTP1_ROLE_SERVER)
  → http1_feed_input / http1_next_event
  → HEADERS_COMPLETE → host-built HTTP/1.1 200 + static body
  → PROTOCOL_EVENT_ERROR → 400 Bad Request
  → close after send
```

- shaggy parses only; **response bytes are host-built** (shaggy server does not serialize replies).
- Current shaggy request-line detector is **GET-oriented** (sibling limitation).
- Partial feeds re-recv until headers complete.

## TLS (ADR-014)

| Host | Library |
|------|---------|
| edgehost | **OpenSSL** non-blocking (P1.13) |
| CPE agent | **mbedTLS** |
| pqproxy side-car | OpenSSL/kTLS |

## Config (ADR-005)

Shadow YAML → `edgecore_apply_config` → CONFIG_APPLIED / REJECTED.

## Deliberate absences

- Real `/health` JSON + metrics (P1.4c), OpenSSL, state store, plugins.

## Related

- Program design: `~/edge-platform-program-design.md`
- shaggy examples: `~/shaggy/examples/liburing_http2_kv.c` (HTTP/2; we use HTTP/1 for P1.4b)
