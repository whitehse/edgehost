# ARCHITECTURE.md — edgehost

## Status

**P1.1**: `libedgecore` skeleton (`create` / `next_event` / fixed event ring).
Host accept loop and plugins not yet present.

## Split (ADR-002)

| Layer | Path | Rules |
|-------|------|-------|
| **libedgecore** | `src/core/*`, `include/edgecore.h` | Syscall-free; pull events; no silent post-create malloc |
| **Host** | `src/host/*` (planned) | io_uring, sockets, files, `host_alloc`, mbedTLS, signals |
| **Plugins** | `src/plugins/*` (planned) | Grand functions (OpenAI first); stubs for Slack/Teams |
| **SPA** | `spa/` (planned) | Static company UI; packages under host config path |

```
edgehost (this repo)
  ├── include/edgecore.h     public core API
  ├── src/core/edgecore.c    event ring + lifecycle (P1.1)
  ├── cmake/Find*.cmake      sibling discovery via *_ROOT / SIBLING_ROOT
  └── deps/pins.txt          known-good git SHAs

siblings (not vendored)
  shaggy, librest, libyaml, pique, libharness, libslack, libteams, libwebmap
  libsim (class A fuzz — P1.5)
```

## edgecore event model

Host drives core with feed APIs (later) and **pulls** via `edgecore_next_event`.
Kinds (P1.1 enum; payloads grow later):

`NONE`, `NEED_ALLOC`, `NEED_REALLOC`, `WANT_SEND`, `HTTP_REQUEST`,
`STATE_CHANGED`, `WS_BROADCAST`, `CONFIG_APPLIED`, `CONFIG_REJECTED`,
`OUTBOUND_DONE` (host-internal), `QUEUE_OVERFLOW`.

## Deliberate absences (P1.1)

- No accept loop, no HTTP bridge, no state store, no plugins.
- No `host_alloc` / NEED_ALLOC yet (P1.2).
- No YAML/HUP (P1.3), no mbedTLS (P1.13), no sim_main fuzz (P1.5).
- No git submodules (pins + local roots instead).

## Related

- Program design: `~/edge-platform-program-design.md` (Track 1, Decision X1, TLS = mbedTLS)
- pqproxy: reference io_uring host pattern (OpenSSL side-car coexistence)
- ADRs: `docs/decisions/`
