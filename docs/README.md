# edgehost documentation

| Doc | Description |
|-----|-------------|
| [../AGENTS.md](../AGENTS.md) | Agent operating contract + pin policy |
| [../ARCHITECTURE.md](../ARCHITECTURE.md) | Module boundaries |
| [DOMAIN.md](DOMAIN.md) | Glossary |
| [../TODO.md](../TODO.md) | Track 1 PR checklist |
| [decisions/](decisions/) | ADRs (write when work lands) |
| [guides/](guides/) | How-tos |
| [../deploy/](../deploy/) | systemd unit + install sketch |

## Guides

| Guide | Title |
|-------|-------|
| [state-namespaces.md](guides/state-namespaces.md) | Extra ns config + ingest hooks (P1.14) |
| [prometheus.md](guides/prometheus.md) | `/health` JSON + scrape notes (P1.15) |

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

Program-wide design: `~/edge-platform-program-design.md`.
