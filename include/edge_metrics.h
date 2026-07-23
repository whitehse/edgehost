/**
 * @file edge_metrics.h
 * @brief Basic host process counters (P1.4c) + memory breakdown for /health.
 *
 * Exposed via GET /health JSON. Native Prometheus text is deferred; see
 * docs/guides/prometheus.md (P1.15).
 */
#ifndef EDGE_METRICS_H
#define EDGE_METRICS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    time_t   started_at;     /* wall clock at metrics init */
    uint64_t accepts;        /* successful accept() completions */
    uint64_t requests;       /* requests that completed headers or errored */
    uint64_t responses_2xx;
    uint64_t responses_4xx;
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint64_t active_conns;   /* currently open client fds */
    uint64_t rejects;        /* accept when table full, etc. */
} edge_metrics_t;

/** Optional subsystem estimates for /health memory.subsystems (not owned). */
typedef struct {
    uint64_t state_rss_bytes;      /* eager state store tables */
    uint64_t e7_rss_estimate;      /* max_sessions * per-session estimate */
    uint64_t e7_sessions_open;
    uint32_t e7_max_sessions;
    int      have_state;
    int      have_e7;
} edge_metrics_memory_extra_t;

void edge_metrics_init(edge_metrics_t *m);

/**
 * Format /health JSON into @p buf (counters only + process/host_alloc memory).
 * @return bytes written (excluding NUL), or -1 if truncated/error.
 */
int edge_metrics_format_health_json(const edge_metrics_t *m, char *buf,
                                    size_t buflen);

/**
 * Format /health JSON with optional subsystem memory estimates.
 * @p extra may be NULL (same as format_health_json for subsystem section).
 */
int edge_metrics_format_health_json_ex(const edge_metrics_t *m,
                                       const edge_metrics_memory_extra_t *extra,
                                       char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_METRICS_H */
