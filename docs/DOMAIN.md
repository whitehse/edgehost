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
| **Shadow config** | Newly loaded YAML not yet applied; discarded on validate failure |
| **CONFIG_APPLIED** | Event after successful `edgecore_apply_config` |
| **CONFIG_REJECTED** | Event when load/validate fails; previous config kept |
| **/health** | `GET` JSON liveness + basic counters (`edge_metrics`) |
| **Metrics** | Host process counters (accepts, requests, 2xx/4xx, bytes, …) |

Public API: `edgecore.h`, `edge_config.h`, `edge_config_hup.h`, `edge_iouring.h`,
`edge_metrics.h`, `host_alloc.h`. Example: `config/edgehost.example.yaml`.
