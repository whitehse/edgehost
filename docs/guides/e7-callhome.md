# E7 NETCONF Call Home (lab)

How to run the **lab** Call Home vertical: raw transport on loopback port
**4334**, Calix-shaped **identity preamble**, NETCONF **CLIENT** after accept,
`lab.v1` notification apply into `net.pon` / `inventory`, SPA at **`/e7/`**,
REST under **`/api/v1/e7/`**.

| Doc | Role |
|-----|------|
| [Design](../designs/e7-netconf-callhome.md) | Full architecture, K-decisions, PR plan |
| [ADR-018](../decisions/018-e7-netconf-callhome.md) | Accepted lab decisions summary |
| `config/edgehost.e7-lab.yaml` | **Lab YAML** — enable this file |

---

## Security banners (read first)

1. **Raw is lab-only.** Production path is **SSH Call Home** (PR-8 / libassh).
2. **Do not** bind `transport: raw` on a reachable interface (`0.0.0.0`, LAN
   IP, etc.) without an explicit **`lab_insecure_raw: true`**. Config validate
   rejects/fails that combination when the flag is false.
3. **Never** ship example configs with `0.0.0.0` + `raw` + open allow-all
   without a lab banner and `lab_insecure_raw: true`.
4. Loopback (`127.0.0.1` / `::1`) + raw does **not** require `lab_insecure_raw`
   (see `config/edgehost.e7-lab.yaml`).
5. REST mutations need lab auth (cookie) with **employee** / **employee_admin**
   as appropriate (ADR-013).

---

## Prerequisites

```bash
cd ~/edgehost
cmake -B build -S . && cmake --build build
# EDGEHOST_HAVE_LIBNETCONF must be on (sibling libnetconf pin)
```

Env for lab auth (same as other lab guides):

```bash
export EDGEHOST_LAB_PASSWORD=lab
export EDGEHOST_SESSION_HMAC='dev-hmac-key-32-bytes-minimum!!'
```

---

## Lab config: `config/edgehost.e7-lab.yaml`

This is the **documented lab profile** for E7 Call Home. Highlights:

| Setting | Lab value | Notes |
|---------|-----------|--------|
| HTTP listen | `127.0.0.1:18080` | SPA + REST |
| `plugins.e7_callhome.enabled` | `true` | Turns on listener + sessions |
| `listen_host` / `listen_port` | `127.0.0.1` / **4334** | Separate Call Home fd |
| `transport` | `raw` | Delimiter framing; no SSH |
| `lab_insecure_raw` | `false` | OK because listen is loopback |
| `reload_policy` | `merge` | YAML MAC wins; runtime-only retained |
| `auto_subscribe_unknown` | `false` | Unknown MAC does not auto-sub |
| `rss_budget_bytes` | `268435456` (256 MiB) | Create fails if estimate exceeds |
| `max_sessions` | `160` | ~150 design + headroom |
| `dirty_cap` | `8192` | K16 coalesce table |
| `shelves[]` | sample MAC `00:02:5d:d9:21:47` | Allowlist seed (K17) |
| `state.namespaces.net_pon` | enabled, `max_keys: 16384` | ONT/PON producer |
| `state.namespaces.inventory` | enabled, `max_keys: 512` | Shelf session/config |
| `state.max_value_bytes` | `2048` | Compact WS envelopes |

Default `config/edgehost.lab.yaml` / `edgehost.example.yaml` keep
`e7_callhome.enabled: false`.

---

## Start the server

```bash
cd ~/edgehost
export EDGEHOST_LAB_PASSWORD=lab
export EDGEHOST_SESSION_HMAC='dev-hmac-key-32-bytes-minimum!!'
./build/edgehost --config config/edgehost.e7-lab.yaml --host 127.0.0.1 --port 18080
```

Expect stderr similar to:

```text
edgehost: e7_callhome listening on 127.0.0.1:4334 (raw)
```

Browser:

| URL | Purpose |
|-----|---------|
| http://127.0.0.1:18080/ | Company home (link to E7) |
| http://127.0.0.1:18080/e7/ | **E7 Call Home SPA** (status, shelves table, ONTs, commands) |
| http://127.0.0.1:18080/lab/ | Generic lab console |
| http://127.0.0.1:18080/health | Process health JSON (no auth) |

Password: `lab` (lab-login cookie).

---

## Session flow (lab raw)

1. Peer connects to **4334**.
2. Peer sends **Calix-shaped identity preamble** (not NETCONF hello), e.g.
   fixture `tests/fixtures/e7/lab_v1_identity.xml`:
   ```xml
   <version>1</version><identity><mac>00:02:5d:d9:21:47</mac>…</identity>
   ```
3. Host parses **MAC** (primary key, K17), matches allowlist, rejects bad/
   unknown/disabled as configured.
4. Host creates libnetconf **`NETCONF_ROLE_CLIENT`** (K13), sends client hello,
   drains peer hello → `SESSION_OPEN`.
5. `create-subscription` for allowlisted (or `auto_subscribe_unknown`) shelves.
6. Notifications run through **lab.v1** apply → `net.pon` keys; inventory
   session/config via put+notify; ONT/PON may **coalesce** WS (≤100 ms).

---

## lab.v1 honesty

> **lab.v1 is not a Calix wire format.**

Phase-1 extractors and fixtures under `tests/fixtures/e7/` use a clear lab
namespace (e.g. `urn:edgehost:lab:e7:1.0`), not forged vendor URIs. They prove
apply paths and SPA/REST until redacted field samples enable `calix.e7.*`.

| Fixture | Role |
|---------|------|
| `lab_v1_identity.xml` | Identity preamble shape for parse/tests |
| `lab_v1_ont_up.xml` / `lab_v1_ont_down.xml` | ONT oper-state |
| `lab_v1_pon_alarm.xml` | PON alarm |

---

## REST table (`/api/v1/e7/*`)

Auth: lab session cookie. **employee+** for reads; **employee_admin** for
mutations (lab password session typically grants admin in lab mode — check
`/auth/me`).

| Method | Path | Purpose |
|--------|------|---------|
| `GET` | `/api/v1/e7/status` | Call Home metrics (accepts, sessions, rejects, coalesce, …) |
| `GET` | `/api/v1/e7/shelves` | Allowlist + live session summary |
| `GET` | `/api/v1/e7/shelves/{mac}` | Detail (identity + session) |
| `PUT` | `/api/v1/e7/shelves/{mac}` | Runtime allowlist upsert (**non-durable**) |
| `DELETE` | `/api/v1/e7/shelves/{mac}` | Remove + disconnect |
| `POST` | `/api/v1/e7/shelves/{mac}/disconnect` | Force close session |
| `GET` | `/api/v1/e7/shelves/{mac}/onts` | Paginated ONT list (`limit`, `cursor`) |
| `POST` | `/api/v1/e7/shelves/{mac}/commands` | Placeholder command (`rpc_xml` body) |
| `GET` | `/api/v1/e7/shelves/{mac}/commands/{cmd_id}` | Poll command result |

Example (after login cookie):

```bash
curl -s -b cookies.txt http://127.0.0.1:18080/api/v1/e7/status | jq .
curl -s -b cookies.txt http://127.0.0.1:18080/api/v1/e7/shelves | jq .
```

State keys (also visible via `/api/v1/state/…`):

| ns | Key pattern |
|----|-------------|
| `inventory` | `e7/{mac-hyphen}/config`, `…/session`, `…/cmd/{id}` |
| `net.pon` | `e7/{mac-hyphen}/ont/{aid}`, `…/pon/{aid}` |

---

## SPA `/e7/`

Static under `spa/e7/`. Features:

- Lab login + auth badge
- **Non-durable allowlist** banner (REST edits lost on restart unless YAML
  updated)
- Status metrics table (`GET /api/v1/e7/status`)
- Shelves table (MAC, label, enabled, session, serial/model)
- Upsert / disconnect / delete
- ONT list + command placeholder

Linked from company home and `/lab/`.

---

## RSS / scale notes

- Create-time check: `max_sessions * edge_e7_session_rss_estimate()` must fit
  `rss_budget_bytes` (default **256 MiB** for session+libnetconf buffers;
  state is separate).
- Mandatory reduced NETCONF profile (K14): `event_queue_size=8`, 256 KiB RPC/
  output caps — **not** library defaults (4 MiB × 160 would blow RSS).
- Coalesce dirty-set: default **8192** keys; flush on tick ≤**100 ms**;
  overflow force-notifies (metrics: coalesce flush / overflow).
- Do not “just set 32k keys” on the global default — use per-ns
  `state.namespaces.net_pon.max_keys` (K10 / ADR-007).
- Soak ~150 peers only after RSS budget pass and cgroup memory limits.

---

## Tests

```bash
cd ~/edgehost/build
ctest -R 'e7' --output-on-failure
# or full suite minus pin verify:
ctest -E edgehost_verify_pins --output-on-failure
```

- `edgehost_e7_event_apply_test` — lab.v1 parsers/fixtures
- `edgehost_e7_callhome_test` — identity, CLIENT path, REST/status helpers

---

## Production (not this guide)

| Item | Status |
|------|--------|
| `transport: ssh` | PR-8 — needs libnetconf libassh |
| Identity before SSH | Still required (K17) |
| Raw on non-loopback | Forbidden without `lab_insecure_raw` (lab only) |
| Durable allowlist | Optional Postgres (PR-10); today YAML + runtime |

Disable: set `plugins.e7_callhome.enabled: false` and restart or SIGHUP apply.
