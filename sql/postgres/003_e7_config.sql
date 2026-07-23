-- edgehost: E7 get-config snapshots + provisioned ONT inventory
-- Apply: psql -h /var/run/postgresql -d edgehost -f sql/postgres/003_e7_config.sql

CREATE SCHEMA IF NOT EXISTS edgehost;

-- Full shelf configuration snapshot (JSON document from XML→JSON)
CREATE TABLE IF NOT EXISTS edgehost.e7_shelf_config (
    id              bigserial PRIMARY KEY,
    shelf_id        text        NOT NULL,
    vendor          text        NOT NULL DEFAULT 'calix',
    source          text        NOT NULL DEFAULT 'get-config/running',
    captured_at     timestamptz NOT NULL DEFAULT now(),
    cmd_id          text,
    xml_bytes       integer     NOT NULL,
    json_bytes      integer     NOT NULL,
    truncated       boolean     NOT NULL DEFAULT false,
    config          jsonb       NOT NULL,
    summary         jsonb       NOT NULL DEFAULT '{}'::jsonb
);

CREATE INDEX IF NOT EXISTS e7_shelf_config_shelf_idx
    ON edgehost.e7_shelf_config (shelf_id, captured_at DESC);

-- Normalized provisioned ONT inventory (query + SPA)
CREATE TABLE IF NOT EXISTS edgehost.e7_ont_provision (
    shelf_id        text        NOT NULL,
    ont_id          text        NOT NULL,
    fsan            text,
    account         text,
    pon_id          text,
    model           text,
    admin_state     text,
    ports           jsonb       NOT NULL DEFAULT '[]'::jsonb,
    raw             jsonb       NOT NULL DEFAULT '{}'::jsonb,
    config_id       bigint      REFERENCES edgehost.e7_shelf_config(id)
                                ON DELETE CASCADE,
    captured_at     timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (shelf_id, ont_id, captured_at)
);

CREATE INDEX IF NOT EXISTS e7_ont_provision_fsan_idx
    ON edgehost.e7_ont_provision (fsan);
CREATE INDEX IF NOT EXISTS e7_ont_provision_account_idx
    ON edgehost.e7_ont_provision (account);
CREATE INDEX IF NOT EXISTS e7_ont_provision_shelf_latest_idx
    ON edgehost.e7_ont_provision (shelf_id, captured_at DESC);

CREATE OR REPLACE VIEW edgehost.e7_ont_provision_latest AS
SELECT DISTINCT ON (shelf_id, ont_id) *
FROM edgehost.e7_ont_provision
ORDER BY shelf_id, ont_id, captured_at DESC;

COMMENT ON TABLE edgehost.e7_shelf_config IS
    'E7 running-config snapshots (get-config) as jsonb';
COMMENT ON TABLE edgehost.e7_ont_provision IS
    'Provisioned ONT account/FSAN/eth services derived from e7_shelf_config';
