# TODO — edgehost

## Done

- [x] **P1.0** deps/pins.txt + CMake Find modules + AGENTS pin policy + deps smoke
- [x] **P1.1** scaffold + edgecore skeleton + ADRs 001–002, 011–012 (`create`/`next_event` smoke)

## Next (program Track 1)

- [ ] **P1.2** host_alloc + NEED_ALLOC path + ADR-003
- [ ] **P1.3** YAML load + SIGHUP apply + ADR-005
- [ ] **P1.4a** io_uring accept + fixed static response
- [ ] **P1.4b** shaggy HTTP/1 parse bridge
- [ ] **P1.4c** `GET /health` JSON + metrics counters
- [ ] **P1.5** sim_main fuzz (needs libsim P0.3)
- [ ] **P1.6** static SPA root + package path
- [ ] **P1.7a–d** state store, WS stream, auth ladder
- [ ] **P1.8a–b** plugin ABI + openai_proxy E2E
- [ ] **P1.9–P1.10** slack/teams stubs
- [ ] **P1.11–P1.12** pqproxy metrics + PG NOTIFY
- [ ] **P1.13 / P1.13b** mbedTLS server + client
- [ ] **P1.14–P1.15** extra ns hooks + deploy notes

## Soft deps

- [x] libsim P0.1–P0.4 (separate repo; pin in deps/pins.txt)
- [ ] Wire class-A fuzz via P1.5 once host accept exists
