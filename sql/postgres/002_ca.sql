-- edgehost Certificate Authority (keys + certs + CRL in Postgres)
-- Apply: psql -h /var/run/postgresql -d edgehost -f sql/postgres/002_ca.sql
-- Unix domain socket is the preferred lab path (no TCP).

CREATE SCHEMA IF NOT EXISTS edgehost;

CREATE TABLE IF NOT EXISTS edgehost.ca_authority (
    id              SERIAL PRIMARY KEY,
    name            TEXT NOT NULL UNIQUE,
    subject_dn      TEXT NOT NULL,
    cert_pem        TEXT NOT NULL,
    key_pem         TEXT NOT NULL,
    serial_next     BIGINT NOT NULL DEFAULT 2,
    crl_number      BIGINT NOT NULL DEFAULT 1,
    active          BOOLEAN NOT NULL DEFAULT TRUE,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS edgehost.ca_certificate (
    id              SERIAL PRIMARY KEY,
    ca_id           INT NOT NULL REFERENCES edgehost.ca_authority(id) ON DELETE CASCADE,
    serial          BIGINT NOT NULL,
    subject_dn      TEXT NOT NULL,
    common_name     TEXT,
    device_id       TEXT,
    not_before      TIMESTAMPTZ NOT NULL,
    not_after       TIMESTAMPTZ NOT NULL,
    cert_pem        TEXT NOT NULL,
    csr_pem         TEXT,
    status          TEXT NOT NULL DEFAULT 'valid'
                    CHECK (status IN ('valid', 'revoked', 'expired')),
    revoked_at      TIMESTAMPTZ,
    revoke_reason   TEXT,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (ca_id, serial)
);

CREATE INDEX IF NOT EXISTS ca_certificate_status_idx
    ON edgehost.ca_certificate (status);
CREATE INDEX IF NOT EXISTS ca_certificate_cn_idx
    ON edgehost.ca_certificate (common_name);
CREATE INDEX IF NOT EXISTS ca_certificate_device_idx
    ON edgehost.ca_certificate (device_id);

CREATE TABLE IF NOT EXISTS edgehost.ca_crl (
    id              SERIAL PRIMARY KEY,
    ca_id           INT NOT NULL REFERENCES edgehost.ca_authority(id) ON DELETE CASCADE,
    crl_number      BIGINT NOT NULL,
    crl_pem         TEXT NOT NULL,
    this_update     TIMESTAMPTZ NOT NULL,
    next_update     TIMESTAMPTZ NOT NULL,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS ca_crl_ca_idx
    ON edgehost.ca_crl (ca_id, crl_number DESC);

COMMENT ON TABLE edgehost.ca_authority IS
    'CA cert + private key PEM (lab: protect DB access; production: HSM/KMS)';
COMMENT ON TABLE edgehost.ca_certificate IS
    'Issued leaf/intermediate certificates and revocation state';
COMMENT ON TABLE edgehost.ca_crl IS
    'Published CRLs served by edgehost at /ca/crl.pem';
