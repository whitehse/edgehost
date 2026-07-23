# Certificate Authority (edgehost)

OpenSSL-based CA for CPE CSRs. Signing keys and issued certificates live in
**Postgres** (Unix domain socket). CRLs are published over the edgehost HTTP
server.

## Architecture

| Piece | Role |
|-------|------|
| OpenSSL | RSA keygen, CSR verify, X509 sign, CRL sign (host CPU) |
| `edge_pg` | Non-blocking Unix-socket simple-query client (`poll`) |
| Postgres | `edgehost.ca_authority`, `ca_certificate`, `ca_crl` |
| SPA `/ca/` | List certs, create CA, sign CSR, issue leaf, revoke, CRL |
| `GET /ca/crl.pem` | Public CRL (no auth) |

## Setup (browser)

```bash
./scripts/run-ca.sh
```

Builds edgehost (unless `NO_BUILD=1`), applies `sql/postgres/002_ca.sql` when
`psql` can reach the Unix socket, starts the CA lab profile, waits for
`/health`, and opens the CA SPA:

| URL | Purpose |
|-----|---------|
| http://127.0.0.1:18080/ca/ | Admin UI (login password: **lab**) |
| http://127.0.0.1:18080/ca/crl.pem | Public CRL |

| Variable | Default | Meaning |
|----------|---------|---------|
| `EDGEHOST_PORT` | `18080` | HTTP port |
| `OPEN_BROWSER` | `1` | `0` = do not launch a browser |
| `APPLY_SCHEMA` | `1` | `0` = skip `psql` schema apply |
| `FOREGROUND` | `1` | `0` = daemonize + print pid |
| `CA_PG_SOCK` / `CA_PG_DATABASE` / `CA_PG_USER` | Unix socket defaults | Postgres target |

## Setup (manual)

```bash
# 1) Schema (as a role that can create tables)
psql -h /var/run/postgresql -d edgehost -f sql/postgres/002_ca.sql

# 2) Use config/edgehost.ca-lab.yaml, or enable plugins.ca in YAML
plugins:
  ca:
    enabled: true
    pg_sock: /var/run/postgresql/.s.PGSQL.5432
    database: edgehost
    user: edgehost
    default_days: 825

# 3) Restart edgehost, open http://127.0.0.1:18080/ca/
```

Local auth: prefer **peer** or **trust** for the Unix socket user. Password auth
uses Postgres cleartext (`auth` method 3); MD5 is not implemented.

## API

| Method | Path | Auth | Purpose |
|--------|------|------|---------|
| GET | `/api/v1/ca/status` | employee | Counts |
| GET | `/api/v1/ca/authorities` | employee | List CAs |
| POST | `/api/v1/ca/authorities` | admin | Create root CA |
| GET | `/api/v1/ca/certs` | employee | List certs (`?status=valid`) |
| GET | `/api/v1/ca/certs/{id}` | employee | Cert detail + PEM |
| POST | `/api/v1/ca/sign` | employee | Sign CSR PEM |
| POST | `/api/v1/ca/issue` | admin | Generate key+leaf (lab) |
| POST | `/api/v1/ca/certs/{id}/revoke` | admin | Revoke + rebuild CRL |
| POST | `/api/v1/ca/crl/rebuild` | admin | Rebuild CRL |
| GET | `/ca/crl.pem` | **public** | Latest CRL PEM |

### Sign CSR example

```bash
curl -sS -b cookies.txt -X POST http://127.0.0.1:18080/api/v1/ca/sign \
  -H 'Content-Type: application/json' \
  -d '{"ca_id":1,"device_id":"cpe-001","days":825,"csr_pem":"-----BEGIN CERTIFICATE REQUEST-----\n..."}'
```

## Security notes

- Private keys are stored as PEM in Postgres — protect DB access (socket
  permissions, role grants). Production should use HSM/KMS.
- Public CRL has no session cookie so devices can fetch it.
- OpenSSL crypto is synchronous on the host thread; Postgres I/O uses
  non-blocking sockets + `poll` (same pattern as TLS helpers).

## Related

- `sql/postgres/002_ca.sql`
- ADR style: OpenSSL non-blocking for wire I/O (ADR-014 lineage)
