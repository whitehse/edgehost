# edgehost

Multi-plugin **io_uring** webserver for the Edge Platform: syscall-free
**edgecore** + Linux host, composing pure-C sibling libraries (shaggy, libyaml,
librest, …).

**Status:** P1.0 — dependency pins and CMake Find modules. No listen socket yet.

## Build

```bash
# siblings expected under $HOME (or set SIBLING_ROOT / *_ROOT)
cmake -B build -S .
cmake --build build
ctest --test-dir build --output-on-failure
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
- Program design: `~/edge-platform-program-design.md`

## License

MIT (same family as sibling libraries).
