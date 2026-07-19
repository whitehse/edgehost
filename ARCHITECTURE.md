# ARCHITECTURE.md — edgehost

## Status

**P1.8a**: state + WS + auth + **plugin ABI / PENDING** (ADR-008).

## Layers

| Layer | Path |
|-------|------|
| **edgecore** | events, config, state, auth_rbac, **pending_table** |
| **HTTP serve** | health, SPA, packages, state, stream, lab-login |
| **plugin_host** | registry, host API v0, PENDING dispatch |
| **io_uring** | production sockets; WS keep-alive |
| **sim_main** | class-A fuzz |

## Plugin ABI (P1.8a)

| Item | Detail |
|------|--------|
| Header | `edge_plugin.h` |
| Kinds | `HTTP` (`on_http` / `on_http_complete`), `SESSION` (feed/next_event) |
| Status | `OK` \| `PENDING` \| `ERR` |
| Host API | state_get/put, log, http_client_request, timers (stub) |
| pending_table | fixed cap; one outbound per inbound slot |
| Real TLS outbound | P1.8b + P1.13b |

## Auth

| Mode | Mechanism |
|------|-----------|
| `open` | no checks (default) |
| `lab_password` | `POST /auth/lab-login` → cookie |
| `proxy_headers` | signed `X-User` headers |

## Next

P1.8b openai_proxy E2E; P1.9–P1.10 stubs; P1.13 TLS.
