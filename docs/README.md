# edgehost documentation

| Doc | Description |
|-----|-------------|
| [../AGENTS.md](../AGENTS.md) | Agent operating contract + pin policy |
| [../ARCHITECTURE.md](../ARCHITECTURE.md) | Module boundaries |
| [DOMAIN.md](DOMAIN.md) | Glossary |
| [../TODO.md](../TODO.md) | Track 1 + E7 Call Home checklist |
| [decisions/](decisions/) | ADRs (write when work lands) |
| [designs/](designs/) | Longer design docs |
| [guides/](guides/) | How-tos |
| [../deploy/](../deploy/) | systemd unit + install sketch |

## Guides

| Guide | Title |
|-------|-------|
| [state-namespaces.md](guides/state-namespaces.md) | Extra ns config + ingest hooks (P1.14) |
| [prometheus.md](guides/prometheus.md) | `/health` JSON + scrape notes (P1.15) |
| [clickhouse.md](guides/clickhouse.md) | ClickHouse events + CPE proxy + Postgres ONT status |
| [ca.md](guides/ca.md) | Certificate Authority (CSR sign, CRL, Postgres keys) |
| [lab-e2e.md](guides/lab-e2e.md) | Lab auth + SPA console + packages E2E |
| [status-map.md](guides/status-map.md) | Login + WebGPU status map (libwebmap demo) |
| [e7-callhome.md](guides/e7-callhome.md) | Call Home lab (Calix + Junos, 4334, SPA `/e7/`) · `run-status-map-e7.sh` / `run-status-map-junos.sh` |
| [e7-config-inventory.md](guides/e7-config-inventory.md) | get-config capture → JSON / ONT account · FSAN · eth services |

## Designs

| Design | Title |
|--------|-------|
| [e7-netconf-callhome.md](designs/e7-netconf-callhome.md) | NETCONF Call Home for Calix E7 (approved rev 4) |

## Decisions

| ADR | Title |
|-----|-------|
| [001](decisions/001-pure-c-choice.md) | Pure C11 |
| [002](decisions/002-core-host-split.md) | Core vs host split |
| [003](decisions/003-event-gated-memory.md) | Event-gated memory (X1) |
| [005](decisions/005-yaml-sighup-apply.md) | YAML + SIGHUP shadow apply |
| [007](decisions/007-state-store.md) | State store net.core / map.dynamic |
| [011](decisions/011-fuzz-and-sim-class-a.md) | Class A fuzz/sim policy |
| [012](decisions/012-agent-ready-documentation.md) | Agent-ready docs |
| [014](decisions/014-tls-openssl-edgehost.md) | edgehost OpenSSL NB; CPE mbedTLS |
| [015](decisions/015-pqproxy-notify.md) | pqproxy scrape + NOTIFY apply |
| [016](decisions/016-extra-ns-deploy.md) | Extra ns config + deploy/Prometheus (P1.14–15) |
| [018](decisions/018-e7-netconf-callhome.md) | E7 NETCONF Call Home lab foundation |
| [019](decisions/019-clickhouse-e7-events.md) | ClickHouse events + Postgres ONT status |

Program-wide design: `~/edge-platform-program-design.md`.
