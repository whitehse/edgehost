/**
 * @file pq_sidecar.c
 * @brief pqproxy metrics scrape → net.core/pqproxy/health (P1.11).
 */

#include "edge_pq_sidecar.h"

#include "edge_outbound.h"
#include "edge_plugin.h"
#include "edge_state_notify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void edge_pq_sidecar_config_defaults(edge_pq_sidecar_config_t *c)
{
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->enabled = 0;
    snprintf(c->metrics_url, sizeof(c->metrics_url),
             "http://127.0.0.1:9108/metrics");
    c->scrape_interval_ms = 5000;
}

static int64_t find_prom_num(const char *text, const char *metric)
{
    const char *p = text;
    size_t mlen = strlen(metric);
    char *end = NULL;
    long long v;

    while ((p = strstr(p, metric)) != NULL) {
        /* skip HELP/TYPE lines */
        if (p > text && p[-1] != '\n' && p != text) {
            p += mlen;
            continue;
        }
        if (strncmp(p, "# ", 2) == 0 ||
            (p == text && p[0] == '#')) {
            p += mlen;
            continue;
        }
        /* metric name then space then number */
        if (p[mlen] == ' ' || p[mlen] == '\t' || p[mlen] == '{') {
            const char *q = p + mlen;
            while (*q && *q != ' ' && *q != '\t' && *q != '\n') {
                q++; /* skip labels */
            }
            while (*q == ' ' || *q == '\t') {
                q++;
            }
            v = strtoll(q, &end, 10);
            if (end != q) {
                return (int64_t)v;
            }
        }
        p += mlen;
    }
    return -1;
}

int edge_pq_sidecar_parse_metrics(const char *text, size_t text_len,
                                  char *health_json, size_t health_cap,
                                  int *out_up, int64_t *out_pool_busy,
                                  int64_t *out_live_backends,
                                  int64_t *out_active_frontends)
{
    char *tmp;
    int64_t live = -1, active = -1, waiters = -1;
    int up;
    time_t now;
    int n;

    if (!text || text_len == 0 || !health_json || health_cap < 32) {
        return -1;
    }
    tmp = (char *)malloc(text_len + 1);
    if (!tmp) {
        return -1;
    }
    memcpy(tmp, text, text_len);
    tmp[text_len] = '\0';

    live = find_prom_num(tmp, "pqproxy_live_backends");
    active = find_prom_num(tmp, "pqproxy_active_frontends");
    waiters = find_prom_num(tmp, "pqproxy_pool_waiters");
    free(tmp);

    /* If we saw any known metric, consider scrape up. */
    up = (live >= 0 || active >= 0 || waiters >= 0) ? 1 : 0;
    if (out_up) {
        *out_up = up;
    }
    if (out_pool_busy) {
        *out_pool_busy = waiters >= 0 ? waiters : 0;
    }
    if (out_live_backends) {
        *out_live_backends = live >= 0 ? live : 0;
    }
    if (out_active_frontends) {
        *out_active_frontends = active >= 0 ? active : 0;
    }

    now = time(NULL);
    n = snprintf(health_json, health_cap,
                 "{\"up\":%s,\"pool_busy\":%lld,\"live_backends\":%lld,"
                 "\"active_frontends\":%lld,\"scraped_at\":%lld}",
                 up ? "true" : "false",
                 (long long)(waiters >= 0 ? waiters : 0),
                 (long long)(live >= 0 ? live : 0),
                 (long long)(active >= 0 ? active : 0), (long long)now);
    if (n < 0 || (size_t)n >= health_cap) {
        return -1;
    }
    return up ? 0 : -1;
}

int edge_pq_sidecar_scrape_once(const edge_pq_sidecar_config_t *cfg,
                                edge_state_store_t *st, edge_ws_hub_t *hub)
{
    edge_http_client_req_t req;
    edge_http_client_result_t res;
    edge_outbound_opts_t oopts;
    uint8_t *body = NULL;
    size_t blen = 0;
    size_t cap = 256 * 1024;
    char health[512];
    edge_state_err_t er;

    if (!cfg || !cfg->enabled || !st || !cfg->metrics_url[0]) {
        return -1;
    }
    body = (uint8_t *)malloc(cap);
    if (!body) {
        return -1;
    }
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.url = cfg->metrics_url;
    edge_outbound_opts_defaults(&oopts);
    oopts.allow_blocking_dns = 1; /* localhost scrape */
    oopts.max_response_body = cap;
    oopts.default_timeout_ms = 3000;

    if (edge_outbound_http_execute(&req, &oopts, &res, body, cap, &blen) != 0 ||
        res.transport_err != 0 || res.status != 200) {
        free(body);
        /* mark down */
        snprintf(health, sizeof(health),
                 "{\"up\":false,\"pool_busy\":0,\"scraped_at\":%lld}",
                 (long long)time(NULL));
        (void)edge_state_put_and_notify(st, hub, "net.core", "pqproxy/health",
                                        health, strlen(health), NULL, 0);
        return -1;
    }

    if (edge_pq_sidecar_parse_metrics((const char *)body, blen, health,
                                      sizeof(health), NULL, NULL, NULL,
                                      NULL) != 0) {
        /* still store partial up=false if parse failed but HTTP ok */
        snprintf(health, sizeof(health),
                 "{\"up\":false,\"error\":\"parse\",\"scraped_at\":%lld}",
                 (long long)time(NULL));
    }
    free(body);

    er = edge_state_put_and_notify(st, hub, "net.core", "pqproxy/health",
                                   health, strlen(health), NULL, 0);
    if (er != EDGE_STATE_OK) {
        return -1;
    }
    return 0;
}
