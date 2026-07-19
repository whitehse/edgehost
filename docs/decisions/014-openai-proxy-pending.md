# ADR-014: openai_proxy PENDING HTTP + outbound client (P1.8b)

## Status

Accepted (P1.8b)

## Date

2026-07-18

## Context

Phase-1 grand function is an OpenAI-compatible proxy. P1.8a defined the plugin
ABI and pending_table; this PR implements the plugin, host outbound client, and
route wiring without inventing a second async model.

## Decision

1. **Plugin** `openai_proxy` (`EDGE_PLUGIN_KIND_HTTP`):
   - Routes: `POST /v1/chat/completions`, `POST /v1/responses`, `GET /v1/models`
   - Auth: `employee` / `employee_admin` / `service_openai` (RBAC `EDGE_RES_OPENAI`)
   - Service principal: `Authorization: Bearer` matching
     `plugins.openai_proxy.service_api_key_env`
   - Builds upstream request with `Authorization: Bearer $OPENAI_API_KEY`
   - Returns `PENDING` after `http_client_request`
2. **Outbound client** (`edge_outbound.h`): blocking HTTP/1.1; HTTPS via OpenSSL.
   - Prefer `upstream_addr` (no DNS)
   - Blocking `getaddrinfo` only when `dns.allow_blocking: true`
   - Buffer full response (no SSE v1)
3. **PENDING completion**: after `on_http` returns PENDING, host calls
   `edge_plugin_host_finish_pending_sync` (blocking outbound then
   `on_http_complete`). True non-blocking TLS on the ring is P1.13b refinement.
4. **Deep-copy** of outbound headers/body at `http_client_request` time so
   plugin stack temps are safe after `on_http` returns.
5. **Open mode**: synthetic `employee` principal for lab so `/v1/*` works without
   cookies when `auth.mode=open`.

## Consequences

- E2E testable with a local plain-HTTP mock and `upstream_addr: 127.0.0.1`.
- Production should set `upstream_addr` or enable only controlled blocking DNS.
- Rate limit is a simple per-process RPM window (refine later).

## Alternatives considered

| Option | Why not |
|--------|---------|
| SESSION kind for OpenAI | Wrong lifecycle; request/response is HTTP PENDING |
| Full non-blocking TLS in this PR | Deferred polish under P1.13b; still uses PENDING path |
| Require libharness for proxy | Soft dep; light JSON validation is enough for E2E |
