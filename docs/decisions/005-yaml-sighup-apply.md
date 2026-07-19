# ADR-005: YAML Config + SIGHUP Shadow Apply

## Status

Accepted

## Date

2026-07-18

## Context

Program Decision X2: configuration is **YAML** via sibling **libyaml**.
**pqproxy** loads YAML at process start only and handles **SIGINT/SIGTERM** —
it does **not** implement SIGHUP reload today. edgehost needs live reload for
lab and production without inventing a second apply path.

## Decision

1. **Format**: YAML; load with libyaml (`yaml_create` / `feed_input` /
   drain events / `yaml_lookup_scalar`), same pattern as
   `pqproxy/src/config_yaml.c`.
2. **Schema (P1.3)**: `listen.host`/`port`, `spa.root`, `http.max_*`,
   `state.namespaces.net_core|map_dynamic.enabled` (underscore keys because
   libyaml knowledge paths split on `.`).
3. **Single apply function**: `edgecore_apply_config` validates and **atomically
   swaps** the active `edge_config_t` inside the core; emits
   `CONFIG_APPLIED` or `CONFIG_REJECTED`.
4. **Shadow reload**: host loads a new config into a stack/local shadow, then
   calls `edgecore_apply_config`. On failure, previous applied config remains.
5. **SIGHUP (new vs pqproxy)**: handler only sets a `sig_atomic_t` flag
   (`edgehost_hup_install` / `edgehost_hup_take`). Host loop (or tests) calls
   `edgehost_reload_config(path)` — **same** load+apply path as startup.
6. **Load errors**: `edgehost_reload_config` emits `CONFIG_REJECTED` via
   `edgecore_notify_config_rejected` without mutating active config.

## Consequences

- Tests exercise reload without a listen socket or full main loop.
- Future `main` / io_uring loop must poll `edgehost_hup_take()` (or equivalent)
  and call `edgehost_reload_config`.
- Namespace YAML keys use underscores (`net_core`); internal ns names remain
  `net.core` / `map.dynamic` when the state store lands (P1.7a).

## Alternatives considered

| Option | Why not |
|--------|---------|
| Claim pqproxy already does HUP | False — only SIGINT/SIGTERM today |
| In-place mutate without events | No observability; harder to test |
| Separate apply paths for HUP vs startup | Drift and double bugs |
| Callback from signal into YAML load | Not async-signal-safe |
