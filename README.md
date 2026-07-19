# edgehost

Multi-plugin **io_uring** webserver for the Edge Platform: syscall-free
**edgecore** + Linux host, composing pure-C sibling libraries (shaggy, libyaml,
librest, …).

**Status:** P1.7d — HTTP/1 + SPA + packages + state REST + WS stream +
**lab session / proxy X-User HMAC** (ADR-013). Default auth mode is `open`.

## Build

```bash
cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure

# open lab (default auth.mode: open):
./build/edgehost --host 127.0.0.1 --port 8080 --config config/edgehost.example.yaml

# lab_password: EDGEHOST_LAB_PASSWORD + EDGEHOST_SESSION_HMAC
#   POST /auth/lab-login → Cookie edge_session
# proxy_headers: EDGEHOST_PROXY_HMAC
#   reverse-proxy injects X-User, X-Auth-Timestamp, X-Auth-Signature
```

## Dependency pins

See [`deps/pins.txt`](deps/pins.txt) and [`AGENTS.md`](AGENTS.md).

```bash
deps/verify_pins.sh
deps/update_pins.sh   # refresh SHAs from local checkouts
```

## Docs

- [AGENTS.md](AGENTS.md) — agent entry
- [ARCHITECTURE.md](ARCHITECTURE.md) — core vs host
- [TODO.md](TODO.md) — PR plan
- [docs/decisions/](docs/decisions/) — ADRs
- Program design: `~/edge-platform-program-design.md`

## License

MIT (same family as sibling libraries).
