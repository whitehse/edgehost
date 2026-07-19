# ADR-013: Auth roles + lab password session (P1.7c)

## Status

Accepted (P1.7c lab session; P1.7d proxy headers)

## Date

2026-07-18

## Context

Program design freezes v1 **roles** across authn mechanisms while allowing a
phased employee ladder: lab password → proxy headers → full OIDC. Full OIDC on
io_uring is out of scope for phase-1 mergeable PRs.

## Decision

1. **Roles** (bit flags in `edge_auth.h`): `employee`, `employee_admin`, `cpe`,
   `customer`, `service_openai`, `ingest`. Stable across steps 1–3.
2. **Principal**: `{ sub, roles, exp, router_ids[]? }` — from session cookie
   (step 1) or proxy headers (step 2). Same RBAC for both.
3. **Pure RBAC** (`auth_rbac.c`): `edge_auth_rbac_check(principal, resource, ns, key)`.
   - `employee` / `employee_admin`: state GET/PUT/DELETE/list, WS stream, packages
   - `ingest`: state PUT only
   - anonymous: deny protected resources
4. **Mode** (`auth.mode` in YAML):
   - `open` (default): no checks — keeps unit tests and lab curl simple
   - `lab_password` (step 1 / P1.7c): enforce session cookie on protected paths
   - `proxy_headers` (step 2 / P1.7d): enforce signed proxy headers
5. **Lab login** (P1.7c): `POST /auth/lab-login` with `{"password":"…"}` →
   `Set-Cookie: edge_session=…` signed with `auth.session_hmac_key_env`.
6. **Proxy headers** (P1.7d): reverse-proxy (oauth2-proxy / nginx auth_request)
   injects and signs:
   - `X-User` — subject (required)
   - `X-Roles` — comma-separated roles (optional; default `employee`)
   - `X-Auth-Timestamp` — unix seconds
   - `X-Auth-Signature` — base64url(HMAC-SHA256(proxy_key, canonical))
   - Canonical: `v1\n{ts}\n{user}\n{roles}`
   - Skew window: `auth.proxy_max_skew_s` (default 300)
   - Key env: `auth.proxy_hmac_key_env` (default `EDGEHOST_PROXY_HMAC`)
7. **Protected when enforced**: `/api/v1/state/*`, `/api/v1/stream`,
   `/packages/*`. Open: `/health`, SPA static, `/auth/lab-login`.
8. **`GET /auth/me`**: returns principal when authn valid; 401 otherwise.
9. Unix-domain peer-only trust deferred; HMAC is the mergeable production path.

## Consequences

- SPA can log in without OIDC; production edge terminates IdP and signs headers.
- Missing secrets with enforced modes fail process start (clear error).
- `Secure` cookie flag deferred until TLS (P1.13).

## Alternatives considered

| Option | Why not |
|--------|---------|
| Always require auth | Breaks existing tests / simple lab demos |
| Full OIDC in P1.7c | Too large for mergeable PR; ladder step 3 later |
| Trust X-User without HMAC | Spoofable unless edge is UDS-only to proxy |
| JWT library | Extra dep; fixed session blob + HMAC is enough for lab |
