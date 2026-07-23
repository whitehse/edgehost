-- edgehost: ONT operational status in Postgres (just-in-time SPA via NOTIFY)
-- Apply: psql "$DATABASE_URL" -f sql/postgres/001_ont_status.sql
--
-- edgehost (or a side-car writer) UPSERTs rows when ONT state changes.
-- LISTEN clients (or pqproxy NOTIFY apply → net.pon) receive compact payloads
-- for browser STATE_CHANGED without polling ClickHouse.

CREATE SCHEMA IF NOT EXISTS edgehost;

CREATE TABLE IF NOT EXISTS edgehost.ont_status (
    shelf_id         TEXT        NOT NULL,
    ont_id           TEXT        NOT NULL,
    pon_id           TEXT,
    -- up | down | error | degraded | unknown
    status           TEXT        NOT NULL DEFAULT 'unknown',
    severity         TEXT,
    last_event_type  TEXT,
    last_event_at    TIMESTAMPTZ,
    details          JSONB       NOT NULL DEFAULT '{}'::jsonb,
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (shelf_id, ont_id)
);

CREATE INDEX IF NOT EXISTS ont_status_pon_idx
    ON edgehost.ont_status (shelf_id, pon_id);

CREATE INDEX IF NOT EXISTS ont_status_status_idx
    ON edgehost.ont_status (status);

CREATE INDEX IF NOT EXISTS ont_status_updated_idx
    ON edgehost.ont_status (updated_at DESC);

COMMENT ON TABLE edgehost.ont_status IS
    'Current ONT oper-state for SPA; history/events live in ClickHouse e7_netconf_events';

-- NOTIFY payload matches edge_notify_apply schema so edgehost can:
--   LISTEN ont_status → edge_notify_apply → net.pon + WS STATE_CHANGED
CREATE OR REPLACE FUNCTION edgehost.notify_ont_status()
RETURNS trigger
LANGUAGE plpgsql
AS $$
DECLARE
    payload jsonb;
    ch text := 'ont_status';
BEGIN
    IF TG_OP = 'DELETE' THEN
        payload := jsonb_build_object(
            'ns', 'net.pon',
            'key', 'e7/' || replace(OLD.shelf_id, ':', '-') || '/ont/' ||
                   replace(OLD.ont_id, '/', '-'),
            'op', 'delete',
            'request_id', 'pg-ont-' || replace(OLD.shelf_id, ':', '')
        );
        PERFORM pg_notify(ch, payload::text);
        RETURN OLD;
    END IF;

    payload := jsonb_build_object(
        'ns', 'net.pon',
        'key', 'e7/' || replace(NEW.shelf_id, ':', '-') || '/ont/' ||
               replace(NEW.ont_id, '/', '-'),
        'op', 'put',
        'value', jsonb_build_object(
            'v', 1,
            'source', 'postgres.ont_status',
            'shelf_id', NEW.shelf_id,
            'ont_id', NEW.ont_id,
            'pon_id', NEW.pon_id,
            'status', NEW.status,
            'severity', NEW.severity,
            'last_event_type', NEW.last_event_type,
            'last_event_at', NEW.last_event_at,
            'details', NEW.details,
            'updated_at', NEW.updated_at
        ),
        'request_id', 'pg-ont-' || replace(NEW.shelf_id, ':', '')
    );
    PERFORM pg_notify(ch, payload::text);
    RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS ont_status_notify ON edgehost.ont_status;
CREATE TRIGGER ont_status_notify
    AFTER INSERT OR UPDATE OR DELETE ON edgehost.ont_status
    FOR EACH ROW EXECUTE FUNCTION edgehost.notify_ont_status();

-- Helper upsert for writers (edgehost later / side-car)
CREATE OR REPLACE FUNCTION edgehost.upsert_ont_status(
    p_shelf_id text,
    p_ont_id text,
    p_pon_id text,
    p_status text,
    p_severity text DEFAULT NULL,
    p_event_type text DEFAULT NULL,
    p_event_at timestamptz DEFAULT now(),
    p_details jsonb DEFAULT '{}'::jsonb
) RETURNS void
LANGUAGE sql
AS $$
    INSERT INTO edgehost.ont_status AS t (
        shelf_id, ont_id, pon_id, status, severity,
        last_event_type, last_event_at, details, updated_at
    ) VALUES (
        p_shelf_id, p_ont_id, p_pon_id, p_status, p_severity,
        p_event_type, p_event_at, COALESCE(p_details, '{}'::jsonb), now()
    )
    ON CONFLICT (shelf_id, ont_id) DO UPDATE SET
        pon_id = COALESCE(EXCLUDED.pon_id, t.pon_id),
        status = EXCLUDED.status,
        severity = COALESCE(EXCLUDED.severity, t.severity),
        last_event_type = COALESCE(EXCLUDED.last_event_type, t.last_event_type),
        last_event_at = COALESCE(EXCLUDED.last_event_at, t.last_event_at),
        details = t.details || EXCLUDED.details,
        updated_at = now();
$$;
