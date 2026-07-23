-- edgehost: E7 NETCONF / CPE telemetry events (ClickHouse)
-- Apply: clickhouse-client --multiquery < sql/clickhouse/001_e7_netconf_events.sql
--
-- Query patterns:
--   WHERE ont_id = '1/1/3/12'
--   WHERE shelf_id = '00:02:5d:d9:21:47' OR shelf_mac = …
--   WHERE pon_id = '1/1/3' AND event_type = 'ont-event'
-- payload is JSON (ClickHouse JSON type) for flexible analysis.

CREATE DATABASE IF NOT EXISTS edgehost;

CREATE TABLE IF NOT EXISTS edgehost.e7_netconf_events
(
    event_time   DateTime64(3, 'UTC'),
    ingested_at  DateTime64(3, 'UTC') DEFAULT now64(3),
    shelf_id     String,
    shelf_mac    String,
    ont_id       String,
    pon_id       String,
    event_type   LowCardinality(String),
    severity     LowCardinality(String),
    source       LowCardinality(String),
    peer         String,
    -- JSON body (XML converted). ClickHouse ≥ 22.3 JSON type; if unavailable
    -- replace with String and query via JSONExtract*.
    payload      JSON,
    xml_raw      String CODEC(ZSTD(3))
)
ENGINE = MergeTree
PARTITION BY toYYYYMM(event_time)
ORDER BY (shelf_id, ont_id, pon_id, event_type, event_time)
TTL toDateTime(event_time) + INTERVAL 180 DAY;

-- Optional projection-friendly view for ONT timeline
-- CREATE VIEW IF NOT EXISTS edgehost.v_ont_events AS
-- SELECT event_time, shelf_id, ont_id, pon_id, event_type, severity, payload
-- FROM edgehost.e7_netconf_events;
