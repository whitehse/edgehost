# ARCHITECTURE.md — edgehost

## Status

**P1.2**: `libedgecore` + event-gated memory (`NEED_ALLOC` / `host_alloc` /
`provide_buffer`). Host accept loop and plugins not yet present.

## Split (ADR-002)

| Layer | Path | Rules |
|-------|------|-------|
| **libedgecore** | `src/core/*`, `include/edgecore.h` | Syscall-free; pull events; no silent post-create malloc for data |
| **Host alloc** | `src/host/host_alloc.c`, `include/host_alloc.h` | Sole process malloc gate for edgecore-owned data buffers |
| **Host** | `src/host/*` (more planned) | io_uring, sockets, files, mbedTLS, signals |
| **Plugins** | `src/plugins/*` (planned) | Grand functions (OpenAI first); stubs for Slack/Teams |
| **SPA** | `spa/` (planned) | Static company UI; packages under host config path |

```
edgehost (this repo)
  ├── include/edgecore.h       public core API
  ├── include/host_alloc.h     host malloc gate
  ├── src/core/edgecore.c      event ring + buffer slots
  ├── src/host/host_alloc.c    malloc/realloc/free + stats
  ├── cmake/Find*.cmake        sibling discovery
  └── deps/pins.txt            known-good git SHAs

siblings (not vendored)
  shaggy, librest, libyaml, pique, libharness, libslack, libteams, libwebmap
  libsim (class A fuzz — P1.5)
```

## Memory model (ADR-003 / X1)

```
request_alloc → NEED_ALLOC event → host_alloc → provide_buffer
request_realloc → NEED_REALLOC → host_realloc → provide_buffer
detach_buffer → host_free
```

- Create may `calloc` core object + fixed event ring only.
- Core stores host pointers; **does not free** them on destroy.
- Siblings keep create-time alloc; pre-size via config caps.

## edgecore event model

Host drives core with feed APIs (later) and **pulls** via `edgecore_next_event`.
Kinds: `NONE`, `NEED_ALLOC`, `NEED_REALLOC`, `WANT_SEND`, `HTTP_REQUEST`,
`STATE_CHANGED`, `WS_BROADCAST`, `CONFIG_APPLIED`, `CONFIG_REJECTED`,
`OUTBOUND_DONE` (host-internal), `QUEUE_OVERFLOW`.

## Deliberate absences (P1.2)

- No accept loop, no HTTP bridge, no state store, no plugins.
- No YAML/HUP (P1.3), no mbedTLS (P1.13), no sim_main fuzz (P1.5).
- No git submodules (pins + local roots instead).

## Related

- Program design: `~/edge-platform-program-design.md` (Track 1, Decision X1, TLS = mbedTLS)
- pqproxy: reference io_uring host pattern
- ADRs: `docs/decisions/`
