# E7 / Junos NETCONF Call Home (lab)

## Juniper Junos Call Home (outbound-ssh)

Same listen port as Calix (default **4334**). Roles match RFC 8071: device
initiates TCP; after the initiation sequence, **edgehost is SSH client and
NETCONF client** (device is SSH/NETCONF server). No Calix-style `<ack>ok</ack>`.

After TCP accept, Junos sends an initiation sequence:

```
MSG-ID: DEVICE-CONN-INFO\r\n
MSG-VER: V1\r\n
DEVICE-ID: <device-id>\r\n
```

When `system services outbound-ssh client <name> secret <shared>` is configured
on the router, the sequence also includes:

```
HOST-KEY: <ssh public host key>\r\n
HMAC:<hex>\r\n
```

Juniper documents this as a **SHA1 hash derived in part from the secret** so the
NMS can verify that the presented host key belongs to the `device-id`. edgehost
verifies **HMAC-SHA1** when the allowlist entry has a shared secret (optional on
both sides). If the allowlist has a secret but the peer omits HOST-KEY/HMAC, the
dial is rejected. If the peer sends HMAC but the allowlist has no secret,
verification is skipped (logged).

Then SSH client (libchssh) + NETCONF client (`NETCONF_ROLE_CLIENT`, subsystem
`netconf`) run on the **same** TCP socket.

### SPA / allowlist

**Add / edit shelf** on `/e7/`:

| Field | Calix | Junos |
|-------|-------|-------|
| Vendor | `calix` | `junos` |
| Path id | MAC | DEVICE-ID (or path id = device-id) |
| Shared secret | n/a | optional; matches Junos `outbound-ssh … secret` |
| DEVICE-ID | n/a | optional if path id already is the device-id |

Secret is never echoed back (`has_secret: true|false` only). Clear with the
“Clear stored secret” checkbox. Persist with
`plugins.e7_callhome.allowlist_path` (`device_id=… vendor=junos secret=…`).

### Junos device sketch

```
set system services netconf ssh
set system services outbound-ssh client edgehost device-id pe1.lab
set system services outbound-ssh client edgehost secret "optional-shared"
set system services outbound-ssh client edgehost services netconf
set system services outbound-ssh client edgehost servers 10.0.0.1 port 4334
```

Reference: [Juniper NETCONF Call Home Sessions](https://www.juniper.net/documentation/us/en/software/junos/netconf/topics/topic-map/netconf-call-home.html).


How to run the **lab** Call Home vertical: raw transport on loopback port
**4334**, Calix-shaped **identity preamble**, NETCONF **CLIENT** after accept,
`lab.v1` notification apply into `net.pon` / `inventory`, SPA at **`/e7/`**,
REST under **`/api/v1/e7/`**.

| Doc | Role |
|-----|------|
| [Design](../designs/e7-netconf-callhome.md) | Full architecture, K-decisions, PR plan |
| [ADR-018](../decisions/018-e7-netconf-callhome.md) | Accepted lab decisions summary |
| `config/edgehost.e7-lab.yaml` | **Lab YAML** (E7 only) |
| `config/edgehost.status-map-e7.yaml` | **Map + E7** combined lab YAML |
| `./scripts/run-status-map-e7.sh` | Start script: map tiles + E7 SPA |

---

## Security banners (read first)

1. **Raw is lab-only.** Production path is **SSH Call Home** (PR-8 / libassh
   when `EDGEHOST_E7_SSH_AVAILABLE=1`).
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
| `transport` | `raw` | Delimiter framing; no SSH (default lab) |
| `ssh_password` / `ssh_username` / `host_key_path` | (empty) | For `transport: ssh` lab; password required for password auth |
| `lab_insecure_raw` | `false` | OK because listen is loopback |
| `reload_policy` | `merge` | YAML MAC wins; runtime-only retained |
| `auto_subscribe_unknown` | `false` | Unknown MAC does not auto-sub |
| `subscription_stream` | `exa-events` | `<stream>` for create-subscription (Calix field) |
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

### Recommended: status map + E7 admin

One process with the **WebGPU status map** and **E7 Call Home** screens:

```bash
cd ~/edgehost
./scripts/run-status-map-e7.sh
# config template: config/edgehost.status-map-e7.yaml
# runtime YAML:    var/edgehost.status-map-e7.runtime.yaml (E7_HOST / E7_PORT applied)
# links libwebmap demo tiles, lab auth env, allowlist at ./var/e7_allowlist.txt
#
# Call Home bind (default all interfaces):
#   E7_HOST=0.0.0.0 E7_PORT=4334 ./scripts/run-status-map-e7.sh
# Bind only one NIC:
#   E7_HOST=192.0.2.10 ./scripts/run-status-map-e7.sh
# HTTP SPA still defaults to 127.0.0.1 (EDGEHOST_HOST / EDGEHOST_PORT).
```

### E7-only lab (no map asset linking)

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
| http://127.0.0.1:18080/ | Company home (links to map + E7) |
| http://127.0.0.1:18080/map/ | **Status map** (WebGPU; `run-status-map-e7.sh`) |
| http://127.0.0.1:18080/e7/ | **E7 Call Home SPA** (status, **connection progress log**, shelves, ONTs, commands) |
| http://127.0.0.1:18080/lab/ | Generic lab console |
| http://127.0.0.1:18080/health | Process health JSON (no auth) |

**Connection progress (debug Inactive shelves):** while logged in, the SPA polls
`GET /api/v1/e7/events?since=<id>` and shows:

- **In-flight sessions** — TCP accepted peers mid identity / SSH / hello
- **Event log** — ring of stages: `accepted` → `identity` → `identity_ok` →
  `allowlist_ok` | `reject_unconfigured` → `ssh` → `nc_state` → `hello` →
  `open` → `subscribe` / timeouts / rejects

Shelf table **Session** column maps `empty` → **Inactive** until NETCONF opens.

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
| `lab_v1_ont_up_geo.xml` | ONT up + lon/lat → also `map.dynamic` (PR-9 partial) |
| `lab_v1_pon_alarm.xml` | PON alarm |

---

## REST table (`/api/v1/e7/*`)

Auth: lab session cookie. **employee+** for reads; **employee_admin** for
mutations. Lab password login (`POST /auth/lab-login`) grants both
`employee` and `employee_admin` so the `/e7/` SPA can upsert shelves (check
`/auth/me`).

| Method | Path | Purpose |
|--------|------|---------|
| `GET` | `/api/v1/e7/status` | Call Home metrics (accepts, sessions, rejects, coalesce, …) |
| `GET` | `/api/v1/e7/events?since=` | Connection progress ring + in-flight sessions (SPA log) |
| `GET` | `/api/v1/e7/shelves` | Allowlist + live session summary |
| `GET` | `/api/v1/e7/shelves/{mac}` | Detail (identity + session) |
| `PUT` | `/api/v1/e7/shelves/{mac}` | Runtime allowlist upsert (file if `allowlist_path`) |
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
| `map.dynamic` | `ont/{mac-hyphen}/{ont-aid}` when notification has lon/lat |

---

## SPA `/e7/`

Static under `spa/e7/`. Features:

- Lab login + auth badge
- **Allowlist durability** banner (`allowlist_path` file or YAML seed;
  updated)
- Status metrics table (`GET /api/v1/e7/status`)
- **Connection progress** log + in-flight sessions (`GET /api/v1/e7/events`)
- Shelves table (MAC, label, enabled, session, serial/model) — `empty` shown as **Inactive**
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

## SIGHUP allowlist (K15)

With `--config PATH`, **SIGHUP** reloads YAML (same path as startup):

1. Shadow load + validate → `edgecore_apply_config` (generation++).
2. `edge_state_apply_config` (namespace enable/capacity).
3. `edge_e7_callhome_apply_config`:
   - **`reload_policy: merge`** (default): YAML MAC entries upsert into the
     runtime allowlist (YAML wins for those MACs); **runtime-only** shelves
     (REST upserts not in YAML) are **retained**.
   - **`replace_all`**: clear runtime table, reseed from YAML only.
4. **Listen host/port/enabled** are **not** rebound live — a warning is logged
   if they change; restart required. Allowlist still applies.

REST allowlist edits are **not written to YAML**. Set
`plugins.e7_callhome.allowlist_path` (text file: `mac=… enabled=… label=…`) to
persist them across restarts (PR-10 interim; Postgres optional later).

---

## Tests

```bash
cd ~/edgehost/build
ctest -R 'e7' --output-on-failure
# or full suite minus pin verify:
ctest -E edgehost_verify_pins --output-on-failure
```

- `edgehost_e7_event_apply_test` — lab.v1 parsers/fixtures (+ geo → map.dynamic)
- `edgehost_e7_callhome_test` — identity, CLIENT path, REST/status, apply_config merge

### Lab e2e script

```bash
./scripts/e7-callhome-e2e.sh
# STRICT=1 fails if ports busy; KEEP_RUNNING=1 leaves edgehost up
```

Starts `config/edgehost.e7-lab.yaml`, lab-login, `GET /api/v1/e7/status`.
Fail-soft (exit 0) if ports are busy or libnetconf is not linked.

---

## SSH Call Home lab (PR-8)

When configured with libassh (`EDGEHOST_E7_SSH_AVAILABLE=1` at build time):

```yaml
plugins:
  e7_callhome:
    transport: ssh
    ssh_password: lab          # lab only; NETCONF_SSH_CALLHOME server auth
    ssh_username: netconf      # optional expected user
    # host_key_path: /path/to/host_key   # empty → ephemeral ed25519
    # ssh_allow_none_auth: false
```

Wire order (Calix): **TCP accept → identity preamble (raw) → SSH (NMS server)
→ NETCONF client** over subsystem `netconf`. Full dialectic SSH coverage lives
in libnetconf `netconf_ssh_test`; edgehost unit tests cover create/bind +
profile with `transport: ssh`.

Without libassh, create/bind for `transport: ssh` fail with a clear stderr
message (`EDGEHOST_E7_SSH_AVAILABLE=0`).

---

## Production notes

| Item | Status |
|------|--------|
| `transport: ssh` | **Implemented** when libassh present; lab password config above |
| Identity before SSH | Required (K17); still raw TCP before SSH |
| Raw on non-loopback | Forbidden without `lab_insecure_raw` (lab only) |
| Durable allowlist | **File** via `allowlist_path` (PR-10 interim); Postgres still optional later |
| Map home outlines | Future; ONT points with coords land in `map.dynamic` now |

Disable: set `plugins.e7_callhome.enabled: false` and restart (or SIGHUP for
allowlist/ns flags; listen disable still needs restart).
