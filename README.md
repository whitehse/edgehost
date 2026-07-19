# edgehost

Multi-plugin **io_uring** webserver for the Edge Platform: syscall-free
**edgecore** + Linux host, composing pure-C sibling libraries (shaggy, libyaml,
librest, …).

**Status:** P1.6 — **io_uring** HTTP/1, `/health`, static **SPA** (`spa/`),
and **packages** (`/packages/…`). Class-A sim fuzz available. TLS later:
**OpenSSL non-blocking**; CPE uses **mbedTLS**.

## Build

```bash
# needs liburing-dev; siblings under $HOME (or SIBLING_ROOT / *_ROOT)
cmake -B build -S .
cmake --build build
ctest --test-dir build --output-on-failure

# run from repo root so ./spa and ./packages resolve
./build/edgehost --host 127.0.0.1 --port 8080 --config config/edgehost.example.yaml
# curl -s http://127.0.0.1:8080/
# curl -s http://127.0.0.1:8080/health
# curl -s http://127.0.0.1:8080/packages/demo.wmap
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
