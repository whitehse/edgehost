# TODO — edgehost

## Done

- [x] **P1.0** deps/pins.txt + CMake Find modules + AGENTS pin policy + deps smoke
- [x] **P1.1** scaffold + edgecore skeleton + ADRs 001–002, 011–012 (`create`/`next_event` smoke)
- [x] **P1.2** host_alloc + NEED_ALLOC/NEED_REALLOC + ADR-003
- [x] **P1.3** YAML load + SIGHUP apply + ADR-005
- [x] **P1.4a** io_uring accept + fixed static response (plain TCP)
- [x] **P1.4b** shaggy HTTP/1 parse bridge (static simple body)
- [x] **P1.4c** `GET /health` JSON + metrics counters
- [x] **P1.5** sim_main class-A fuzz path (libsim + edge_http1_serve)
- [x] **P1.6** static SPA root + package path (`/packages/`)
- [x] **P1.7a** state store (`net.core`, `map.dynamic`) + REST GET/PUT/DELETE
- [x] **P1.7b** WS `/api/v1/stream` fan-out STATE_CHANGED
- [x] **P1.7c** auth_rbac + lab password session (ADR-013)
- [x] **P1.7d** proxy `X-User` HMAC session (ADR-013 step 2)
- [x] **P1.8a** plugin ABI HTTP/SESSION + PENDING + host API v0 (ADR-008)
- [x] **P1.8b** openai_proxy E2E via PENDING + outbound HTTP(S) (ADR-014)
- [x] **P1.9** slack SESSION stub (`enabled: false` by default)
- [x] **P1.10** teams SESSION stub (`enabled: false` by default)
- [x] **P1.13** OpenSSL non-blocking TLS server (`SSL_set_fd` + io_uring POLL)
- [x] **P1.11** pqproxy side-car metrics scrape → `net.core/pqproxy/health`
- [x] **P1.12** NOTIFY payload schema → state put/delete + WS fan-out
- [x] **P1.13b** OpenSSL non-blocking outbound TLS client (poll WANT_*)
- [x] **P1.14** extra ns config enable + ingest hooks docs (`edge_state_apply_config`)
- [x] **P1.15** deploy unit + Prometheus notes (`deploy/`, `docs/guides/`)

## Next (program Track 1)

- [x] Track 1 complete through P1.15 (see program design for Track 2+)

## Lab E2E (post–Track 1)

- [x] `config/edgehost.lab.yaml` — lab_password + SPA + packages
- [x] Lab SPA console (login, state, WS, packages)
- [x] `packages/fixture_basemap/` + `packages/index.json`
- [x] `scripts/lab-e2e.sh` + `docs/guides/lab-e2e.md` (includes mobile sync)

## Soft deps

- [x] libsim P0.1–P0.4 (separate repo; pin in deps/pins.txt)
- [x] Class-A fuzz path (P1.5); optional BUILD_FUZZ harness

## E7 NETCONF Call Home

Design: `docs/designs/e7-netconf-callhome.md` · ADR-018 · guide
`docs/guides/e7-callhome.md` · lab YAML `config/edgehost.e7-lab.yaml`.

### Foundation (implemented)

- [x] **PR-1** `edge_state_put_and_notify` + plugin hub wiring (K6)
- [x] **PR-2** FindLibnetconf + `plugins.e7_callhome` YAML skeleton
- [x] **PR-2a** Per-ns capacity + enable-time tables (K10 / ADR-007)
- [x] **PR-3** lab.v1 ONT/PON apply + fixtures (`tests/fixtures/e7/`)
- [x] **PR-4a** Call Home listen + identity preamble + raw CLIENT pump
- [x] **PR-4b** Subscribe + lab.v1 apply + coalesced WS (K16)
- [x] **PR-5** REST `/api/v1/e7/*` + command placeholder
- [x] **PR-6** SPA `/e7/`
- [x] **PR-7** Docs / ADR-018 / lab guide / indexes

### Deferred / gated

- [ ] **PR-8** SSH Call Home (libnetconf libassh milestone)
- [ ] **PR-9** Map phase 2 ONT points
- [ ] **PR-10** Optional Postgres durable allowlist
- [ ] Real Calix notification samples → `calix.e7.*` extractors
