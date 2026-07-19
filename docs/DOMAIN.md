# DOMAIN.md — edgehost glossary

| Term | Meaning |
|------|---------|
| **edgecore** | Syscall-free core library: connections, HTTP bridge, router, state, plugins as pure SMs |
| **Host** | Process that owns io_uring, sockets, files, malloc for core buffers, TLS, signals |
| **Pin** | Git SHA of a sibling repo known-good with this edgehost revision (`deps/pins.txt`) |
| **Plugin** | Loaded grand-function module (OpenAI proxy phase-1; Slack/Teams stubs) |
| **State namespace** | Keyed store domain (`net.core`, `map.dynamic`, …) |
| **SIGHUP apply** | Reload YAML via shadow config + atomic swap (new vs pqproxy) |
| **Class A host** | Full io_uring sim path (edgehost + libsim) |
| **Pull event** | Host drains core via `edgecore_next_event` (no callbacks) |
| **NEED_ALLOC** | Core event requesting host-owned buffer growth (P1.2; X1) |

Public API (P1.1): `include/edgecore.h` — `edgecore_create`, `edgecore_next_event`,
`edgecore_destroy`. See program design for full sketches (`edge_plugin.h`, …).
