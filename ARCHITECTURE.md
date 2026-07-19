# ARCHITECTURE.md — edgehost

## Status

**P1.13b**: Track 1 nearly complete — pqproxy scrape, NOTIFY, TLS client.

## Layers

| Layer | Path |
|-------|------|
| **edgecore** | events, config, state, auth_rbac, **pending_table** |
| **HTTP serve** | health, SPA, packages, state, stream, lab-login |
| **plugin_host** | registry, host API v0, PENDING dispatch |
| **io_uring** | production sockets; WS; optional **OpenSSL TLS** |
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

## Next

P1.14–P1.15 deploy notes / extra ns hooks.
