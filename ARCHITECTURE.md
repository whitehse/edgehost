# ARCHITECTURE.md — edgehost

## Status

**P1.3**: edgecore + host_alloc + YAML/HUP config apply. No listen socket yet.

## Split (ADR-002)

| Layer | Path | Rules |
|-------|------|-------|
| **libedgecore** | `src/core/*`, `include/edgecore.h` | Syscall-free; pull events; no silent post-create malloc for data |
| **Config values** | `include/edge_config.h`, `src/core/edge_config.c` | Defaults + validate (pure) |
| **YAML + HUP** | `src/host/config_yaml.c`, `config_hup.c` | File I/O, libyaml, SIGHUP flag |
| **Host alloc** | `src/host/host_alloc.c` | Process malloc for edgecore data buffers |
| **Host** | `src/host/*` (more planned) | io_uring, sockets, files, mbedTLS, signals |
| **Plugins** | `src/plugins/*` (planned) | Grand functions |
| **SPA** | `spa/` (planned) | Static company UI |

## Config flow (ADR-005)

```
startup / SIGHUP flag
  → edge_config_load_yaml_path (shadow)
  → edgecore_apply_config
       validate OK → atomic swap → CONFIG_APPLIED
       validate fail / load fail → keep previous → CONFIG_REJECTED
```

- pqproxy: YAML at start only; **no** SIGHUP reload — HUP is **new** here.
- Handler is flag-only (`edgehost_hup_install` / `edgehost_hup_take`).

## Memory model (ADR-003 / X1)

```
request_alloc → NEED_ALLOC → host_alloc → provide_buffer
detach_buffer → host_free
```

## Deliberate absences (P1.3)

- No accept loop / HTTP bridge (P1.4a+).
- No state store (P1.7a), plugins, mbedTLS, sim_main.

## Related

- Program design: `~/edge-platform-program-design.md`
- Example: `config/edgehost.example.yaml`
- ADRs: `docs/decisions/`
