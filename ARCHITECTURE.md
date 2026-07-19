# ARCHITECTURE.md — edgehost

## Status

**P1.6**: io_uring HTTP/1, `/health` metrics, **static SPA** under `spa.root`,
and **package files** under `packages.root` at URL `/packages/…`.

## Split

| Layer | Path | Rules |
|-------|------|-------|
| **libedgecore** | `src/core/*` | Syscall-free SMs |
| **HTTP/1 serve** | `edge_http1_serve.c` | Route + metrics; calls static loader |
| **Static files** | `static_files.c` | Safe join, MIME, size cap (host I/O) |
| **io_uring** | `iouring_loop.c` | Real sockets; sets docroots from config |
| **sim_main** | `sim_main.c` | Class A fuzz (no SPA cwd dependency) |

## HTTP routes

| Path | Response |
|------|----------|
| `GET /health` | JSON metrics |
| `GET /packages/…` | file under `packages.root` |
| `GET /…` | file under `spa.root` ( `/` → `index.html` ) |
| SPA miss | try `index.html` (client router) then 404 |
| parse error | 400 |

Path traversal (`..`) is rejected.

## Config (YAML)

```yaml
spa:
  root: ./spa
  max_file_bytes: 65536
packages:
  root: ./packages
```

## TLS (ADR-014)

edgehost → OpenSSL non-blocking (P1.13); CPE → mbedTLS.

## Next

P1.7a state store (`net.core`, `map.dynamic`) + REST GET/PUT.
