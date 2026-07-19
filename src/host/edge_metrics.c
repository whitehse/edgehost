/**
 * @file edge_metrics.c
 * @brief Process counters + /health JSON formatter (P1.4c).
 */

#include "edge_metrics.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

void edge_metrics_init(edge_metrics_t *m)
{
    if (!m) {
        return;
    }
    memset(m, 0, sizeof(*m));
    m->started_at = time(NULL);
}

int edge_metrics_format_health_json(const edge_metrics_t *m, char *buf,
                                    size_t buflen)
{
    time_t now;
    long uptime;
    int n;

    if (!m || !buf || buflen < 32) {
        return -1;
    }
    now = time(NULL);
    uptime = (now >= m->started_at) ? (long)(now - m->started_at) : 0;

    n = snprintf(buf, buflen,
                 "{"
                 "\"status\":\"ok\","
                 "\"uptime_s\":%ld,"
                 "\"accepts\":%llu,"
                 "\"requests\":%llu,"
                 "\"responses_2xx\":%llu,"
                 "\"responses_4xx\":%llu,"
                 "\"bytes_in\":%llu,"
                 "\"bytes_out\":%llu,"
                 "\"active_conns\":%llu,"
                 "\"rejects\":%llu"
                 "}",
                 uptime,
                 (unsigned long long)m->accepts,
                 (unsigned long long)m->requests,
                 (unsigned long long)m->responses_2xx,
                 (unsigned long long)m->responses_4xx,
                 (unsigned long long)m->bytes_in,
                 (unsigned long long)m->bytes_out,
                 (unsigned long long)m->active_conns,
                 (unsigned long long)m->rejects);
    if (n < 0 || (size_t)n >= buflen) {
        return -1;
    }
    return n;
}
