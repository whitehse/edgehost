# edgehost

Multi-plugin **io_uring** webserver for the Edge Platform: syscall-free
**edgecore** + Linux host, composing pure-C sibling libraries (shaggy, libyaml,
librest, …).

**Status:** P1.7c — HTTP/1 + SPA + packages + state REST + WS stream +
**lab password session / RBAC** (ADR-013). Default config mode is `open`.

## Build

```bash
cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure

# open lab (default auth.mode: open):
./build/edgehost --host 127.0.0.1 --port 8080 --config config/edgehost.example.yaml
curl -s http://127.0.0.1:8080/health
curl -s -X PUT http://127.0.0.1:8080/api/v1/state/net.core/router/r1 \
  -H 'Content-Type: application/json' -d '{"id":"r1","status":"ok"}'

# lab_password mode:
# export EDGEHOST_LAB_PASSWORD=… EDGEHOST_SESSION_HMAC=…
# set auth.mode: lab_password in YAML
# curl -c jar -X POST …/auth/lab-login -d '{"password":"…"}'
# curl -b jar …/api/v1/state/…
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
