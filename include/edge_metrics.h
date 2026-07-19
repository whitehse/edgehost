/**
 * @file edge_metrics.h
 * @brief Basic host process counters (P1.4c).
 *
 * Exposed via GET /health JSON. Not Prometheus yet (P1.15 notes).
 */
#ifndef EDGE_METRICS_H
#define EDGE_METRICS_H

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

void edge_metrics_init(edge_metrics_t *m);

/**
 * Format /health JSON into @p buf.
 * @return bytes written (excluding NUL), or -1 if truncated/error.
 */
int edge_metrics_format_health_json(const edge_metrics_t *m, char *buf,
                                    size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_METRICS_H */
