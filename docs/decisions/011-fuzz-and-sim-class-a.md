# ADR-011: Fuzz and Sim — Class A Host

## Status

Accepted. **P1.5 landed:** `edge_sim_drive` + `fuzz/fuzz_edgehost_a.c`.

## Date

2026-07-18

## Context

edgehost production uses Linux **io_uring**. Program design Decision X3 defines
three host classes; edgehost is **class A** (full sim including `sim_uring`).
libsim (Track 0) provides `sim_clock` / `sim_timer` / `sim_net` / `sim_uring`
and `sim_fuzz_drive_a`.

## Decision

1. **Class A only** for production edgehost and its primary fuzz path.
2. Drive edgecore + HTTP/1 through **buffers** under libsim (no real sockets
   in unit/fuzz hosts). Shared `edge_http1_serve` is used by both production
   io_uring and sim.
3. **`edge_sim_drive`** (`src/host/sim_main.c`):
   - `sim_fuzz_drive_a` (full class-A opcode stream)
   - edgecore `NEED_ALLOC` + `host_alloc` + `apply_config`
   - direct `edge_http1_serve_feed`
   - mini `sim_net` + `sim_uring` accept/recv/send of HTTP bytes
4. **libFuzzer** harness: `fuzz/fuzz_edgehost_a.c` via `-DBUILD_FUZZ=ON` (clang).
5. Pin libsim via `deps/pins.txt`; do not vendor sim sources.
6. Must not abort on adversarial input; return 0 / surface events.

## Consequences

- `ctest` includes `edgehost_sim_main` when libsim is linked.
- Class B/C consumers use libsim with `LIBSIM_NO_URING` — out of scope here.
- Production `iouring_loop.c` maps CQEs → `edge_http1_serve_feed`.

## Alternatives considered

| Option | Why not |
|--------|---------|
| Real-network-only integration tests | Too slow/flaky for core SM coverage |
| Callback-based sim hooks inside core | Violates ADR-002 pull model |
| Class B epoll sim for edgehost | Mismatches production io_uring semantics |
