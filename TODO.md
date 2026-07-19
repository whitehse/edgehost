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

## Next (program Track 1)

- [ ] **P1.7c–d** auth_rbac + lab session / proxy HMAC
- [ ] **P1.8a–b** plugin ABI + openai_proxy E2E
- [ ] **P1.9–P1.10** slack/teams stubs
- [ ] **P1.11–P1.12** pqproxy metrics + PG NOTIFY
- [ ] **P1.13 / P1.13b** OpenSSL non-blocking server + client (not mbedTLS)
- [ ] **P1.14–P1.15** extra ns hooks + deploy notes

## Soft deps

- [x] libsim P0.1–P0.4 (separate repo; pin in deps/pins.txt)
- [x] Class-A fuzz path (P1.5); optional BUILD_FUZZ harness
