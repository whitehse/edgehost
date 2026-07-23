# E7 config inventory (get-config → JSON → ONT services)

Capture Calix E7 **running configuration** over an OPEN Call Home session,
store it as **JSON**, and enumerate provisioned ONTs with **account number**,
**FSAN / serial**, and **Ethernet port services**.

## Operator flow

### Automatic capture

edgehost submits `get-config` (running) automatically when:

1. **Shelf first connects** — after NETCONF `SESSION_OPEN` (and after
   create-subscription succeeds when allowlisted), with a short delay so the
   session is settled.
2. **Config change / save events** — NETCONF notifications whose name or
   category looks like a configuration change (e.g. `configuration-change`,
   `config-save`, category `CONFIGURATION`). Debounced (**5 s**) so a burst of
   save events becomes one capture.

Log markers: `e7_auto_capture reason=session_open|subscribed|config_event`.

Manual capture still works for on-demand refresh.

### Manual capture

1. Log in on `/e7/` (lab password / employee role).
2. Wait until the shelf session is **OPEN**.
3. Open **Config inventory**, select the shelf MAC, click **Capture running config**.
4. **Poll capture** until status is `ok`, then **Load inventory**.
5. Search by account or FSAN; download full JSON if needed.

## NETCONF

edgehost sends the standard client helper (inner body, not the outer `<rpc>`):

```xml
<get-config>
  <source><running/></source>
</get-config>
```

Equivalent REST:

```http
POST /api/v1/e7/shelves/{mac}/config/capture
```

or the generic command path:

```http
POST /api/v1/e7/shelves/{mac}/commands
Content-Type: application/json

{"op":"get-config"}
```

## Storage

| Layer | What |
|-------|------|
| Disk | `var/e7_config/{mac-key}/latest.xml`, `latest.json`, `latest_inventory.json` |
| Postgres | `edgehost.e7_shelf_config` (full `config` jsonb) + `edgehost.e7_ont_provision` |
| State | Compact meta `net.pon e7/{mac}/config/meta` and per-ONT `…/ontcfg/{ont}` |

Apply schema:

```bash
psql -h /var/run/postgresql -d edgehost -f sql/postgres/003_e7_config.sql
```

Postgres uses the same Unix-socket credentials as the CA plugin when set
(`plugins.ca.pg_*`); otherwise defaults (`edgehost` / trust).

## REST

| Method | Path | Role |
|--------|------|------|
| POST | `/api/v1/e7/shelves/{mac}/config/capture` | start get-config |
| GET | `/api/v1/e7/shelves/{mac}/commands/{cmd_id}` | poll status / meta |
| GET | `/api/v1/e7/shelves/{mac}/config` | latest meta |
| GET | `/api/v1/e7/shelves/{mac}/config/onts` | inventory array |
| GET | `/api/v1/e7/shelves/{mac}/config/full` | full JSON document |

## Field map (Calix AXOS-R24 live + synthetic)

Ground truth from a redacted AXOS-R24.3.0 `get-config` (`product/version`).

| Concept | AXOS tags (live) | Also accepted |
|---------|------------------|---------------|
| ONT id | `ont/ont-id` | |
| FSAN | **`vendor-id` + `serial-number`** → e.g. `CXNK`+`DFB749`=`CXNKDFB749` | bare `fsan` / `serial-number` |
| Account | `subscriber-id` | `account`, `subscriber-location-id` |
| Model / profile | `profile-id` (e.g. `803G`) | `ont-type`, `model` |
| Eth port | `interface/ont-ethernet/port` (`g1`…) | `ont-port`, `eth-port` |
| UA / voice bridge | `interface/ont-ua/id` | |
| POTS | `interface/pots/port` | |
| HSI service | `ont-ethernet/vlan` + `c-vlan` + `policy-map/name` (`100M_100M`, `1G_1G`) | legacy `eth-svc` / `data-svc` |
| Voice service | `ont-ua/vlan/policy-map` name `Voice`, or `sip-service` | `pots-svc` |

Fixtures:

- `tests/fixtures/e7/calix_axos_running_config_sample.xml` — redacted AXOS slice
- `tests/fixtures/e7/calix_e7_running_config_sample.xml` — synthetic EXA-shaped

Optional developer check: place a full dump at `/tmp/e7_config.txt` and run
`edgehost_e7_config_xml_test` (live path asserts ≥200 ONTs).

**Live redacted dump stats** (`/tmp/e7_config.txt`, AXOS-R24.3.0, ~745 KiB XML):

| Metric | Value |
|--------|------:|
| ONTs | 226 |
| With account (`subscriber-id`) | 223 |
| Eth HSI-like services (`policy-map`) | ~179–192 |
| FSAN form | `CXNK` + 6-hex serial |

## Size limits

- libnetconf large replies: heap-backed beyond 64 KiB inline; free via
  `netconf_rpc_reply_take_xml` / `release`.
- E7 session `max_rpc_size`: **2 MiB** (cap for capture **input** growth).
- E7 `max_output_size`: **256 KiB** (create-time outbound buffer; not 2 MiB).
- RSS budget model charges **steady-state** per session (~1.2 MiB), not
  `2 MiB × max_sessions`, so lab `rss_budget_bytes: 256 MiB` still admits
  160 sessions. A rare get-config may grow one session’s input toward 2 MiB.
- Capture command deadline: **120 s**.
- State store never holds full config XML (meta only).

## Tests

```bash
cmake --build build --target edgehost_e7_config_xml_test
ctest --test-dir build -R e7_config_xml --output-on-failure
```
