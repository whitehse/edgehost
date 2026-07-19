# ADR-013: Auth roles + lab password session (P1.7c)

## Status

Accepted (P1.7c)

## Date

2026-07-18

## Context

Program design freezes v1 **roles** across authn mechanisms while allowing a
phased employee ladder: lab password → proxy headers → full OIDC. Full OIDC on
io_uring is out of scope for phase-1 mergeable PRs.

## Decision

1. **Roles** (bit flags in `edge_auth.h`): `employee`, `employee_admin`, `cpe`,
   `customer`, `service_openai`, `ingest`. Stable across steps 1–3.
2. **Principal**: `{ sub, roles, exp, router_ids[]? }` carried in a signed
   session cookie after login.
3. **Pure RBAC** (`auth_rbac.c`): `edge_auth_rbac_check(principal, resource, ns, key)`.
   - `employee` / `employee_admin`: state GET/PUT/DELETE/list, WS stream, packages
   - `ingest`: state PUT only
   - anonymous: deny protected resources
4. **Mode** (`auth.mode` in YAML):
   - `open` (default): no checks — keeps unit tests and lab curl simple
   - `lab_password` (step 1): enforce session on protected paths
   - `proxy_headers` (step 2 / P1.7d): not implemented yet
5. **Lab login**: `POST /auth/lab-login` with `{"password":"…"}` compared to
   env named by `auth.lab_password_env` (default `EDGEHOST_LAB_PASSWORD`).
   Success → `Set-Cookie: edge_session=<b64url(payload)>.<b64url(hmac-sha256)>`
   with `HttpOnly; Path=/; SameSite=Lax; Max-Age=…`.
6. **HMAC key** from env `auth.session_hmac_key_env` (default
   `EDGEHOST_SESSION_HMAC`). Crypto is pure C (no OpenSSL yet; TLS is P1.13).
7. **Protected when lab_password**: `/api/v1/state/*`, `/api/v1/stream`,
   `/packages/*`. Open: `/health`, SPA static, `/auth/lab-login`.
8. **`GET /auth/me`**: returns principal when cookie valid; 401 otherwise.

## Consequences

- SPA can log in without OIDC; production will swap authn without changing RBAC.
- Missing secrets with `mode: lab_password` fail process start (clear error).
- `Secure` cookie flag deferred until TLS (P1.13).

## Alternatives considered

| Option | Why not |
|--------|---------|
| Always require auth | Breaks existing tests / simple lab demos |
| Full OIDC in P1.7c | Too large for mergeable PR; ladder step 3 later |
| JWT library | Extra dep; fixed session blob + HMAC is enough for lab |
