/**
 * @file edge_metrics.c
 * @brief Process counters + /health JSON formatter with memory breakdown.
 */

#include "edge_metrics.h"

#include "host_alloc.h"

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
    return edge_metrics_format_health_json_ex(m, NULL, buf, buflen);
}

int edge_metrics_format_health_json_ex(const edge_metrics_t *m,
                                       const edge_metrics_memory_extra_t *extra,
                                       char *buf, size_t buflen)
{
    time_t now;
    long   uptime;
    char   mem[1400];
    char   sub[256];
    int    mn;
    int    n;

    if (!m || !buf || buflen < 64) {
        return -1;
    }
    now = time(NULL);
    uptime = (now >= m->started_at) ? (long)(now - m->started_at) : 0;

    mn = host_mem_format_json(mem, sizeof(mem));
    if (mn < 0) {
        snprintf(mem, sizeof(mem),
                 "{\"process\":{},\"host_alloc\":{\"bytes\":0,\"by_kind\":{}}}");
    }

    sub[0] = '\0';
    if (extra && (extra->have_state || extra->have_e7)) {
        int sn;
        if (extra->have_state && extra->have_e7) {
            sn = snprintf(sub, sizeof(sub),
                          ",\"subsystems\":{"
                          "\"state_rss_bytes\":%llu,"
                          "\"e7_rss_estimate\":%llu,"
                          "\"e7_sessions_open\":%llu,"
                          "\"e7_max_sessions\":%u"
                          "}",
                          (unsigned long long)extra->state_rss_bytes,
                          (unsigned long long)extra->e7_rss_estimate,
                          (unsigned long long)extra->e7_sessions_open,
                          extra->e7_max_sessions);
        } else if (extra->have_state) {
            sn = snprintf(sub, sizeof(sub),
                          ",\"subsystems\":{"
                          "\"state_rss_bytes\":%llu"
                          "}",
                          (unsigned long long)extra->state_rss_bytes);
        } else {
            sn = snprintf(sub, sizeof(sub),
                          ",\"subsystems\":{"
                          "\"e7_rss_estimate\":%llu,"
                          "\"e7_sessions_open\":%llu,"
                          "\"e7_max_sessions\":%u"
                          "}",
                          (unsigned long long)extra->e7_rss_estimate,
                          (unsigned long long)extra->e7_sessions_open,
                          extra->e7_max_sessions);
        }
        if (sn < 0 || (size_t)sn >= sizeof(sub)) {
            sub[0] = '\0';
        }
    }

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
                 "\"rejects\":%llu,"
                 "\"memory\":%s%s"
                 "}",
                 uptime,
                 (unsigned long long)m->accepts,
                 (unsigned long long)m->requests,
                 (unsigned long long)m->responses_2xx,
                 (unsigned long long)m->responses_4xx,
                 (unsigned long long)m->bytes_in,
                 (unsigned long long)m->bytes_out,
                 (unsigned long long)m->active_conns,
                 (unsigned long long)m->rejects, mem, sub);
    if (n < 0 || (size_t)n >= buflen) {
        return -1;
    }
    return n;
}
