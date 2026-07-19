# ARCHITECTURE.md — edgehost

## Status

P1.0 skeleton: dependency discovery only. Runtime architecture lands in P1.1+.

## Split (program design)

| Layer | Path (planned) | Rules |
|-------|----------------|-------|
| **libedgecore** | `src/core/*` | Syscall-free; pull events; no silent post-create malloc |
| **Host** | `src/host/*` | io_uring, sockets, files, `host_alloc`, mbedTLS, signals |
| **Plugins** | `src/plugins/*` | Grand functions (OpenAI first); stubs for Slack/Teams |
| **SPA** | `spa/` | Static company UI; packages under host config path |

## Dependencies (P1.0)

```
edgehost (this repo)
  ├── cmake/Find*.cmake  → sibling checkouts via *_ROOT / SIBLING_ROOT
  └── deps/pins.txt      → known-good git SHAs (CI / verify_pins.sh)

siblings (not vendored)
  shaggy, librest, libyaml, pique, libharness, libslack, libteams, libwebmap
  libsim (Track 0 — optional until P0.1)
```

## Deliberate absences (P1.0)

- No accept loop, no HTTP bridge, no state store, no plugins.
- No mbedTLS wiring yet (P1.13).
- No git submodules (pins + local roots instead).

## Related

- Program design: `~/edge-platform-program-design.md` (Track 1, Decision X1, TLS = mbedTLS)
- pqproxy: reference io_uring host pattern (OpenSSL side-car coexistence)
