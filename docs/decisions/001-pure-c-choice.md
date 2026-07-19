# ADR-001: Pure C Choice

## Status

Accepted

## Date

2026-07-18

## Context

edgehost must compose pure-C sibling libraries (shaggy, libyaml, librest,
pique, libsim, …), run under Linux io_uring with OpenSSL non-blocking TLS
(later), and stay embeddable in the same agent-driven C11 workflow as the rest
of the Edge Platform.

## Decision

Implement **libedgecore** and the edgehost process in **pure C11**. No C++
runtime, no mandatory language extensions beyond what Linux/liburing already
require on the host side (`CMAKE_C_EXTENSIONS ON` only for host code that
needs them). Public headers stay FFI-friendly (Hanson's opaque types).

## Consequences

- Static link and OpenWrt-class toolchains stay simple.
- Agents and humans share one language surface with siblings.
- Host-only Linux APIs (io_uring, signals) live in `src/host/*`, not in core.

## Alternatives considered

| Option | Why not |
|--------|---------|
| C++17 | ABI / runtime inconsistency with siblings |
| Rust core + C FFI | Extra pin surface; program standard is pure C |
