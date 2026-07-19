# ADR-014: edgehost TLS = OpenSSL non-blocking; CPE = mbedTLS

## Status

Accepted — policy (2026-07-18); **server implementation landed in P1.13**.

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

### P1.13 server implementation

- `edge_tls.h` / `tls_server.c`: `SSL_CTX` from PEM cert/key; `SSL_set_fd` accept.
- `iouring_loop`: after accept, optional `CS_TLS_HS` + `OP_POLL`; then
  `SSL_read` / `SSL_write` with want-read/write polling.
- Config: `tls.cert` + `tls.key` (empty ⇒ plain TCP lab mode remains).
- Optional `tls.client_ca` for mTLS (verify peer).
- Outbound client HTTPS remains blocking OpenSSL from P1.8b; true non-blocking
  client polish is **P1.13b**.

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
