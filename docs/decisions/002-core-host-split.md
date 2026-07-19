# ADR-002: Core vs Host Split

## Status

Accepted

## Date

2026-07-18

## Context

The Edge Platform inherits a non-negotiable sibling contract: **library cores
are syscall-free and pull-event**, while applications own I/O. pqproxy vs
pique is the reference pattern. edgehost must host many plugins and compose
many siblings without turning any one library into a webserver.

## Decision

Split the tree in-process:

| Layer | Path | Owns |
|-------|------|------|
| **libedgecore** | `src/core/*`, `include/edgecore.h` | Connections, HTTP bridge drivers, router, state SMs, plugin SMs as pure machines; **pull** events via `edgecore_next_event`; **no** sockets/files/clocks/signals |
| **Host** | `src/host/*` | io_uring accept/read/write, static files, YAML load + SIGHUP, **OpenSSL non-blocking** (P1.13), signals, process `malloc` for edgecore buffers (`host_alloc` — P1.2) |
| **Plugins** | `src/plugins/*` | Grand functions linked into the host; edgecore sees buffer-level kinds only |

API spine (grows by PR):

- P1.1: `edgecore_create` / `edgecore_destroy` / `edgecore_next_event`
- P1.2: `request_alloc` / `request_realloc` / `provide_buffer` + `host_alloc`
- P1.3: `edgecore_apply_config` + host YAML/HUP (`edgehost_reload_config`)
- P1.4a: `edge_iouring_run` plain TCP static response
- P1.4b: shaggy `http1_feed_input` / `next_event` bridge; host-built reply
- Later: state, auth, OpenSSL TLS, plugin pending HTTP

Event kinds are defined in `edgecore.h` (program design). Host consumes
`EDGE_EVENT_OUTBOUND_DONE` internally; SPA clients never see it.

## Consequences

- Core unit tests and fuzz drivers need no real network.
- Host is the only place for Linux-specific failure modes.
- Scoped X1 (ADR-003 / P1.2): edgecore emits `NEED_ALLOC` rather than silent
  post-create `malloc`; siblings keep their own create-time alloc policies.

## Alternatives considered

| Option | Why not |
|--------|---------|
| edgecore as separate git repo | Extra pin for an app-private core; keep monorepo app + static lib until productized |
| Fork pqproxy into multi-plugin host | Dilutes RLS/mTLS review surface of the Postgres proxy |
| Callbacks from core to host | Rejected; sibling lineage is pull-event only |
