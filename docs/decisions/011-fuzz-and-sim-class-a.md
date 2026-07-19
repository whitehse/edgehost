# ADR-011: Fuzz and Sim — Class A Host

## Status

Accepted (policy). Harness lands in **P1.5** (`sim_main` + libsim).

## Date

2026-07-18

## Context

edgehost production uses Linux **io_uring**. Program design Decision X3 defines
three host classes; edgehost is **class A** (full sim including `sim_uring`).
libsim (Track 0) provides `sim_clock` / `sim_timer` / `sim_net` / `sim_uring`
and `sim_fuzz_drive_a`.

## Decision

1. **Class A only** for production edgehost and its primary fuzz path.
2. Drive edgecore through **buffers + pull events** under libsim (no real
   sockets in unit/fuzz hosts).
3. Prefer **libFuzzer + ASan/UBSan** harnesses once `sim_main` exists (`BUILD_FUZZ`).
4. Pin libsim via `deps/pins.txt`; do not vendor sim sources.
5. Core must not abort on adversarial feed data; surface `EDGE_EVENT_*` /
   return codes instead.

P1.1 does **not** ship a fuzz binary — only this policy so later PRs do not
invent a second I/O model.

## Consequences

- P1.5 depends on libsim P0.3+ and edgecore accept/feed spine (P1.4a).
- Class B/C consumers (CPE agent, mobile) use libsim with `LIBSIM_NO_URING`
  and are out of scope for this ADR.
- Host `iouring_loop.c` stays thin: map CQEs → `feed_*` / drain `next_event`.

## Alternatives considered

| Option | Why not |
|--------|---------|
| Real-network-only integration tests | Too slow/flaky for core SM coverage |
| Callback-based sim hooks inside core | Violates ADR-002 pull model |
| Class B epoll sim for edgehost | Mismatches production io_uring semantics |
