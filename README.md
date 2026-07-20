# edgehost

Multi-plugin **io_uring** webserver for the Edge Platform: syscall-free
**edgecore** + Linux host, composing pure-C sibling libraries (shaggy, libyaml,
librest, …).

**Status:** Track 1 complete through **P1.15** — state (extra ns config), WS,
auth, openai_proxy, TLS, pqproxy scrape, NOTIFY, deploy unit, Prometheus notes.

## Build

```bash
cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure

# open lab (default auth.mode: open):
./build/edgehost --host 127.0.0.1 --port 8080 --config config/edgehost.example.yaml

# full lab E2E (auth + SPA console + packages + state + mobile sync):
./scripts/lab-e2e.sh
# docs: docs/guides/lab-e2e.md
# browser lab API console: KEEP_RUNNING=1 ./scripts/lab-e2e.sh → http://127.0.0.1:18080/lab/

# status map (login + WebGPU map, links libwebmap demo assets):
./scripts/run-status-map.sh
# docs: docs/guides/status-map.md
# browser: http://127.0.0.1:18080/  → login → /map/

# status map + Calix E7 Call Home (map + /e7/ admin, Call Home 0.0.0.0:4334):
./scripts/run-status-map-e7.sh
# E7_HOST=0.0.0.0 E7_PORT=4334 ./scripts/run-status-map-e7.sh   # Call Home bind
# EDGEHOST_HOST=127.0.0.1 EDGEHOST_PORT=18080                    # HTTP SPA bind
# docs: docs/guides/status-map.md · docs/guides/e7-callhome.md
# browser: /map/ status map · /e7/ shelves, sessions, ONTs, commands

# lab_password manual:
#   EDGEHOST_LAB_PASSWORD + EDGEHOST_SESSION_HMAC
#   ./build/edgehost --config config/edgehost.lab.yaml
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

## Deploy

See [`deploy/README.md`](deploy/README.md) (systemd unit + install sketch).
Metrics: [`docs/guides/prometheus.md`](docs/guides/prometheus.md).
Namespaces: [`docs/guides/state-namespaces.md`](docs/guides/state-namespaces.md).

## Docs

- [AGENTS.md](AGENTS.md) — agent entry
- [ARCHITECTURE.md](ARCHITECTURE.md) — core vs host
- [TODO.md](TODO.md) — PR plan
- [docs/decisions/](docs/decisions/) — ADRs
- Program design: `~/edge-platform-program-design.md`

## License

MIT (same family as sibling libraries).
