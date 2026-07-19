/**
 * @file edge_pq_sidecar.h
 * @brief Scrape pqproxy side-car /metrics into net.core (P1.11).
 *
 * Does not link pqproxy. HTTP GET metrics_url → parse Prometheus text →
 * state put net.core/pqproxy/health (+ optional gauges).
 */
#ifndef EDGE_PQ_SIDECAR_H
#define EDGE_PQ_SIDECAR_H

#include "edge_state.h"
#include "edge_ws.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGE_PQ_METRICS_URL_MAX 256

typedef struct {
    int      enabled;
    char     metrics_url[EDGE_PQ_METRICS_URL_MAX]; /* default http://127.0.0.1:9108/metrics */
    uint32_t scrape_interval_ms;                   /* default 5000 */
} edge_pq_sidecar_config_t;

void edge_pq_sidecar_config_defaults(edge_pq_sidecar_config_t *c);

/**
 * Parse Prometheus text body into health JSON and counters.
 * @return 0 ok, -1 parse failure.
 */
int edge_pq_sidecar_parse_metrics(const char *text, size_t text_len,
                                  char *health_json, size_t health_cap,
                                  int *out_up, int64_t *out_pool_busy,
                                  int64_t *out_live_backends,
                                  int64_t *out_active_frontends);

/**
 * HTTP GET metrics_url, parse, put net.core/pqproxy/health, broadcast WS.
 * @p hub may be NULL.
 * @return 0 ok, -1 scrape/put failure.
 */
int edge_pq_sidecar_scrape_once(const edge_pq_sidecar_config_t *cfg,
                                edge_state_store_t *st, edge_ws_hub_t *hub);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_PQ_SIDECAR_H */
