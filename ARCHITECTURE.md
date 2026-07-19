# ARCHITECTURE.md — edgehost

## Status

**P1.15**: Track 1 complete — extra ns config, deploy unit, Prometheus notes.

**E7 Call Home (lab foundation)**: host module + REST + SPA; design
[docs/designs/e7-netconf-callhome.md](docs/designs/e7-netconf-callhome.md),
ADR-018, guide [docs/guides/e7-callhome.md](docs/guides/e7-callhome.md).

## Layers

| Layer | Path |
|-------|------|
| **edgecore** | events, config, state, auth_rbac, **pending_table** |
| **HTTP serve** | health, SPA, packages, state, stream, lab-login, **`/api/v1/e7/*`** |
| **plugin_host** | registry, host API v0, PENDING dispatch |
| **io_uring** | production sockets; WS; optional **OpenSSL TLS**; E7 Call Home tick |
| **e7_callhome** | second listen fd (raw **4334** lab); identity → NETCONF CLIENT; lab.v1 apply |
| **stubs** | slack / teams SESSION plugins (`enabled: false`) |
| **sim_main** | class-A fuzz |

## Plugin ABI (P1.8a)

| Item | Detail |
|------|--------|
| Header | `edge_plugin.h` |
| Kinds | `HTTP` (`on_http` / `on_http_complete`), `SESSION` (feed/next_event) |
| Status | `OK` \| `PENDING` \| `ERR` |
| Host API | state_get/put, log, http_client_request, timers (stub) |
| pending_table | fixed cap; one outbound per inbound slot |
| Outbound | `outbound_http` blocking HTTP/HTTPS (OpenSSL); addr override preferred |
| openai_proxy | `POST /v1/chat/completions`, `/v1/responses`, `GET /v1/models` |

## Auth

| Mode | Mechanism |
|------|-----------|
| `open` | no checks (default) |
| `lab_password` | `POST /auth/lab-login` → cookie |
| `proxy_headers` | signed `X-User` headers |

## TLS

| Item | Detail |
|------|--------|
| Library | OpenSSL (not mbedTLS) |
| Mode | `SSL_set_fd` + io_uring `POLL` want-read/write |
| Config | `tls.cert` + `tls.key` (omit for plain lab TCP) |
| CPE | mbedTLS in agent tree only |

## Side-car + NOTIFY

| Item | Detail |
|------|--------|
| pqproxy | scrape `/metrics` → `net.core/pqproxy/health` |
| NOTIFY | JSON schema apply → state + WS `STATE_CHANGED` |

## State namespaces (P1.14)

| ns | Default |
|----|---------|
| `net.core`, `map.dynamic` | enabled |
| `net.pon`, `net.home`, `electric`, `inventory` | disabled until config |
| `ext.*` | dynamic via `edge_state_ns_set_enabled` |

`edge_state_apply_config` applies YAML flags on store attach.

## Deploy (P1.15)

| Item | Path |
|------|------|
| systemd unit | `deploy/edgehost.service` |
| install notes | `deploy/README.md` |
| Prometheus | `docs/guides/prometheus.md` (JSON `/health`; text `/metrics` deferred) |

## E7 NETCONF Call Home (lab)

Device-initiated Call Home (RFC 8071). Lab: **raw** delimiter framing on a
dedicated listen port; production **SSH** deferred (PR-8).

| Item | Detail |
|------|--------|
| Design / ADR | `docs/designs/e7-netconf-callhome.md`, ADR-018 |
| Lab guide | `docs/guides/e7-callhome.md` |
| Lab YAML | `config/edgehost.e7-lab.yaml` (`enabled: true`, `127.0.0.1:4334`) |
| Host | `src/host/e7_callhome.c`, `e7_event_apply.c` |
| Headers | `include/edge_e7_callhome.h`, `edge_e7_event_apply.h` |
| SPA | `spa/e7/` → `/e7/` |
| Role (K13) | Host accept + identity; libnetconf **`NETCONF_ROLE_CLIENT`** |
| Identity (K17) | Primary key = MAC from Calix pre-SSH identity XML |
| State | `net.pon` (ONT/PON), `inventory` (session/config/cmd); put + coalesce notify |
| Security | No `0.0.0.0` + raw without `lab_insecure_raw: true` |
