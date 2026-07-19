# ADR-014: edgehost TLS = OpenSSL non-blocking; CPE = mbedTLS

## Status

Accepted (policy; implementation P1.13 / P1.13b for edgehost; CPE when agent needs TLS)

## Date

2026-07-18

## Context

An earlier program draft chose mbedTLS for edgehost. The product owner
corrected the split: the **primary server** should use **OpenSSL in
non-blocking mode** (io_uring-friendly, aligned with pqproxy), while the
**CPE agent** should use **mbedTLS** for OpenWrt-class footprint.

## Decision

| Host | TLS | Mode |
|------|-----|------|
| **edgehost** | **OpenSSL** | Non-blocking: `SSL_set_fd` + io_uring `POLL` / want-read/write (pqproxy lineage) |
| **CPE agent** (`netforensics/agent`) | **mbedTLS** | As required by libuv agent loop |
| **pqproxy** side-car | OpenSSL/kTLS | Unchanged; not linked into edgehost |

- P1.4a remains **plain TCP** only.
- P1.13 / P1.13b add OpenSSL server + client for edgehost.
- Do **not** pull mbedTLS into the edgehost binary for production TLS.
- Do **not** pull OpenSSL into the CPE agent package for production TLS.

## Consequences

- Skill reuse with pqproxy TLS/POLL patterns on the server.
- Honest dual stack across fleet: server OpenSSL, CPE mbedTLS.
- Program design Key Decision 21 and TLS sections updated accordingly.

## Alternatives considered

| Option | Why not |
|--------|---------|
| mbedTLS on edgehost | Rejected by product; weaker fit with existing io_uring+OpenSSL server work |
| OpenSSL on CPE | Larger OpenWrt image; conflicts with CPE size goals |
| Single TLS for all hosts | Different constraints; forces compromise |
