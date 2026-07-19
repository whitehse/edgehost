# edgehost

Multi-plugin **io_uring** webserver for the Edge Platform: syscall-free
**edgecore** + Linux host, composing pure-C sibling libraries (shaggy, libyaml,
librest, …).

**Status:** P1.7b — HTTP/1 + SPA + packages + **state store REST** +
**WebSocket** `GET /api/v1/stream` fan-out of `STATE_CHANGED`. Auth open in
lab until P1.7c.

## Build

```bash
cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure

# from repo root:
./build/edgehost --host 127.0.0.1 --port 8080 --config config/edgehost.example.yaml
curl -s http://127.0.0.1:8080/health
curl -s -X PUT http://127.0.0.1:8080/api/v1/state/net.core/router/r1 \
  -H 'Content-Type: application/json' -d '{"id":"r1","status":"ok"}'
curl -s http://127.0.0.1:8080/api/v1/state/net.core/router/r1
# WS clients: GET /api/v1/stream?topics=state with Upgrade: websocket
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
