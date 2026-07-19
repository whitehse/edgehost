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
| **NEED_ALLOC** | Core event requesting a new host-owned buffer (ADR-003 / X1) |
| **NEED_REALLOC** | Core event requesting grow of an existing host buffer |
| **host_alloc** | Sole process malloc gate for edgecore data (`include/host_alloc.h`) |
| **provide_buffer** | Host hands a fulfilled allocation back to core |

Public API: `include/edgecore.h` (create, next_event, request_alloc,
provide_buffer, …) and `include/host_alloc.h`. See program design for full
sketches (`edge_plugin.h`, …).
