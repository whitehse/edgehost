# edgehost

Multi-plugin **io_uring** webserver for the Edge Platform: syscall-free
**edgecore** + Linux host, composing pure-C sibling libraries (shaggy, libyaml,
librest, …).

**Status:** P1.4b — plain TCP **io_uring** + **shaggy HTTP/1** parse;
static `ok` body until `/health` JSON (P1.4c). TLS later: **OpenSSL
non-blocking**; CPE agent uses **mbedTLS**.

## Build

```bash
# needs liburing-dev; siblings under $HOME (or SIBLING_ROOT / *_ROOT)
cmake -B build -S .
cmake --build build
ctest --test-dir build --output-on-failure

./build/edgehost --host 127.0.0.1 --port 8080
# curl -v http://127.0.0.1:8080/
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
