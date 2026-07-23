/**
 * @file edge_clickhouse.h
 * @brief edgehost ClickHouse integration: E7 events + CPE telemetry proxy.
 *
 * Batches JSONEachRow inserts into ClickHouse (clickhouse-async). E7 NETCONF
 * notifications are converted XML→JSON and enqueued with shelf/ONT/PON/event
 * columns for analysis. CPE devices may POST telemetry to
 * /api/v1/telemetry/events which shares the same aggregator.
 */
#ifndef EDGE_CLICKHOUSE_H
#define EDGE_CLICKHOUSE_H

#include "edge_config.h"

#include "clickhouse/clickhouse-async.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGE_CH_JSON_MAX 65536

typedef struct edge_clickhouse edge_clickhouse_t;

/**
 * Create from edge_config (no-op NULL if clickhouse.enabled is false).
 * Does not take ownership of cfg.
 */
edge_clickhouse_t *edge_clickhouse_create(const edge_config_t *cfg);

void edge_clickhouse_destroy(edge_clickhouse_t *ch);

int edge_clickhouse_enabled(const edge_clickhouse_t *ch);

/** Host tick: time-based batch flush. */
void edge_clickhouse_on_tick(edge_clickhouse_t *ch, uint64_t mono_ms);

/**
 * Enqueue one E7 NETCONF notification for ClickHouse.
 * Extracts shelf_id/mac, ont_id, pon_id, event_type; converts XML→JSON payload.
 * @return 0 queued/flushed, -1 disabled or error.
 */
int edge_clickhouse_enqueue_e7_event(edge_clickhouse_t *ch,
                                     const char *shelf_mac,
                                     const char *shelf_id, const char *peer,
                                     const char *notification_xml,
                                     size_t xml_len, const char *source);

/**
 * Enqueue a pre-built telemetry JSON object (must be a single JSON object).
 * Optional table override (NULL → configured events table).
 */
int edge_clickhouse_enqueue_json(edge_clickhouse_t *ch, const char *table,
                                 const char *json, size_t json_len);

/**
 * Enqueue CPE agent NDJSON (cpe_perf / cpe_nat / cpe_wifi / …) by wrapping
 * into the e7_netconf_events row shape (payload = original object).
 * If the object already looks like an e7 row (has event_type + shelf_id),
 * inserts as-is via enqueue_json.
 */
int edge_clickhouse_enqueue_cpe_ndjson(edge_clickhouse_t *ch, const char *json,
                                       size_t json_len);

/** Force flush. */
int edge_clickhouse_flush(edge_clickhouse_t *ch);

void edge_clickhouse_stats(const edge_clickhouse_t *ch, ch_async_stats_t *out);

/**
 * Convert a simple element-oriented XML fragment to a JSON object string.
 * Text-only elements → string values; nested elements → objects; repeated
 * sibling tags → last-wins (good enough for NETCONF notification bodies).
 * @return bytes written excl NUL, or -1.
 */
int edge_xml_to_json(const char *xml, size_t xml_len, char *out, size_t out_sz);

/**
 * Extract common E7 identifiers from notification XML (lab.v1 + Calix-ish).
 * Empty strings when missing. Always NUL-terminates outputs.
 */
void edge_e7_event_extract_ids(const char *xml, size_t xml_len, char *ont_id,
                               size_t ont_sz, char *pon_id, size_t pon_sz,
                               char *event_type, size_t type_sz,
                               char *severity, size_t sev_sz,
                               char *event_time, size_t et_sz);

/** Status JSON for /api/v1/debug/clickhouse or health extras. */
int edge_clickhouse_status_json(const edge_clickhouse_t *ch, char *buf,
                                size_t buf_sz);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_CLICKHOUSE_H */
