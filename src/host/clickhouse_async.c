/**
 * @file clickhouse_async.c
 * @brief ClickHouse HTTP JSONEachRow async batch client.
 */

#define _GNU_SOURCE

#include "clickhouse/clickhouse-async.h"

#include "edge_outbound.h"
#include "edge_plugin.h"
#include "host_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct ch_async_client {
    ch_async_options_t opt;
    char               table[CH_ASYNC_TABLE_MAX];
    char              *batch; /* host_alloc */
    size_t             batch_cap;
    size_t             batch_len;
    size_t             pending_rows;
    uint64_t           last_flush_ms;
    ch_async_stats_t   stats;
};

void ch_async_options_defaults(ch_async_options_t *o)
{
    if (!o) {
        return;
    }
    memset(o, 0, sizeof(*o));
    snprintf(o->host, sizeof(o->host), "%s", "127.0.0.1");
    o->port = 8123;
    snprintf(o->database, sizeof(o->database), "%s", "edgehost");
    snprintf(o->user, sizeof(o->user), "%s", "default");
    o->timeout_ms = 15000;
    o->flush_interval_ms = 1000;
    o->flush_max_rows = 256;
    o->flush_max_bytes = 256u * 1024u;
}

static uint64_t mono_ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

ch_async_client_t *ch_async_create(const ch_async_options_t *opt)
{
    ch_async_client_t *c;
    size_t cap;

    if (!opt || (!opt->host[0] && !opt->base_url[0])) {
        return NULL;
    }
    c = (ch_async_client_t *)host_alloc_kind(EDGE_MEM_OTHER, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->opt = *opt;
    if (c->opt.port == 0) {
        c->opt.port = c->opt.use_https ? 8443 : 8123;
    }
    if (c->opt.flush_interval_ms == 0) {
        c->opt.flush_interval_ms = 1000;
    }
    if (c->opt.flush_max_rows == 0) {
        c->opt.flush_max_rows = 256;
    }
    if (c->opt.flush_max_bytes == 0) {
        c->opt.flush_max_bytes = 256u * 1024u;
    }
    if (c->opt.timeout_ms == 0) {
        c->opt.timeout_ms = 15000;
    }
    cap = c->opt.flush_max_bytes + 4096u;
    if (cap < 8192u) {
        cap = 8192u;
    }
    c->batch = (char *)host_alloc_kind(EDGE_MEM_OTHER, cap);
    if (!c->batch) {
        host_free(c);
        return NULL;
    }
    c->batch_cap = cap;
    c->last_flush_ms = mono_ms_now();
    return c;
}

void ch_async_destroy(ch_async_client_t *c)
{
    if (!c) {
        return;
    }
    /* Best-effort flush with short timeout so destroy cannot hang. */
    if (c->pending_rows > 0) {
        uint32_t saved = c->opt.timeout_ms;
        c->opt.timeout_ms = 250;
        (void)ch_async_flush(c);
        c->opt.timeout_ms = saved;
    }
    host_free(c->batch);
    host_free(c);
}

void ch_async_stats(const ch_async_client_t *c, ch_async_stats_t *out)
{
    if (!out) {
        return;
    }
    if (!c) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = c->stats;
}

size_t ch_async_pending_rows(const ch_async_client_t *c)
{
    return c ? c->pending_rows : 0;
}

static int build_insert_url(const ch_async_client_t *c, const char *table,
                            char *url, size_t url_sz)
{
    char qtable[CH_ASYNC_TABLE_MAX + CH_ASYNC_DB_MAX + 8];
    char enc[512];
    size_t i, o = 0;
    const char *t = table && table[0] ? table : "e7_netconf_events";

    if (strchr(t, '.')) {
        snprintf(qtable, sizeof(qtable), "%s", t);
    } else if (c->opt.database[0]) {
        snprintf(qtable, sizeof(qtable), "%s.%s", c->opt.database, t);
    } else {
        snprintf(qtable, sizeof(qtable), "%s", t);
    }

    /* minimal query encode for INSERT INTO t FORMAT JSONEachRow */
    {
        char raw[384];
        int n = snprintf(raw, sizeof(raw),
                         "INSERT INTO %s FORMAT JSONEachRow", qtable);
        if (n < 0 || (size_t)n >= sizeof(raw)) {
            return -1;
        }
        for (i = 0; raw[i] && o + 4 < sizeof(enc); i++) {
            unsigned char ch = (unsigned char)raw[i];
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
                ch == '-') {
                enc[o++] = (char)ch;
            } else if (ch == ' ') {
                enc[o++] = '+';
            } else {
                int w = snprintf(enc + o, sizeof(enc) - o, "%%%02X", ch);
                if (w < 0 || (size_t)w >= sizeof(enc) - o) {
                    return -1;
                }
                o += (size_t)w;
            }
        }
        enc[o] = '\0';
    }

    if (c->opt.base_url[0]) {
        size_t blen = strlen(c->opt.base_url);
        int need_slash = (blen > 0 && c->opt.base_url[blen - 1] != '/');
        int n = snprintf(url, url_sz, "%s%s?query=%s", c->opt.base_url,
                         need_slash ? "/" : "", enc);
        return (n < 0 || (size_t)n >= url_sz) ? -1 : 0;
    }
    {
        int n = snprintf(url, url_sz, "http%s://%s:%u/?query=%s",
                         c->opt.use_https ? "s" : "", c->opt.host,
                         (unsigned)c->opt.port, enc);
        return (n < 0 || (size_t)n >= url_sz) ? -1 : 0;
    }
}

int ch_async_flush(ch_async_client_t *c)
{
    edge_http_client_req_t req;
    edge_http_client_result_t res;
    edge_outbound_opts_t oopts;
    char url[768];
    uint8_t resp[2048];
    size_t resp_len = 0;
    int rc;

    if (!c) {
        return -1;
    }
    if (c->pending_rows == 0 || c->batch_len == 0) {
        return 0;
    }
    if (build_insert_url(c, c->table, url, sizeof(url)) != 0) {
        snprintf(c->stats.last_error, sizeof(c->stats.last_error),
                 "build insert url failed");
        c->stats.flush_err++;
        return -1;
    }

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.url = url;
    req.body = (const uint8_t *)c->batch;
    req.body_len = c->batch_len;
    req.timeout_ms = c->opt.timeout_ms;
    /* Optional X-ClickHouse-User / Key via headers when credentials set. */
    {
        static const char *hn[2];
        static const char *hv[2];
        static char user_hdr[CH_ASYNC_USER_MAX];
        static char key_hdr[CH_ASYNC_PASSWORD_MAX];
        size_t nh = 0;
        if (c->opt.user[0]) {
            snprintf(user_hdr, sizeof(user_hdr), "%s", c->opt.user);
            hn[nh] = "X-ClickHouse-User";
            hv[nh] = user_hdr;
            nh++;
        }
        if (c->opt.password[0]) {
            snprintf(key_hdr, sizeof(key_hdr), "%s", c->opt.password);
            hn[nh] = "X-ClickHouse-Key";
            hv[nh] = key_hdr;
            nh++;
        }
        if (nh) {
            req.hdr_names = hn;
            req.hdr_values = hv;
            req.n_headers = nh;
        }
    }

    edge_outbound_opts_defaults(&oopts);
    oopts.allow_blocking_dns = c->opt.allow_blocking_dns;
    oopts.default_timeout_ms = c->opt.timeout_ms;
    oopts.max_response_body = sizeof(resp);

    memset(&res, 0, sizeof(res));
    rc = edge_outbound_http_execute(&req, &oopts, &res, resp, sizeof(resp),
                                    &resp_len);
    c->last_flush_ms = mono_ms_now();
    if (rc != 0 || res.transport_err != 0) {
        snprintf(c->stats.last_error, sizeof(c->stats.last_error),
                 "transport err=%d", res.transport_err);
        c->stats.flush_err++;
        return -1;
    }
    c->stats.last_http_status = res.status;
    if (res.status < 200 || res.status >= 300) {
        size_t n = resp_len < 120 ? resp_len : 120;
        char snippet[128];
        memcpy(snippet, resp, n);
        snippet[n] = '\0';
        snprintf(c->stats.last_error, sizeof(c->stats.last_error),
                 "HTTP %u: %s", (unsigned)res.status, snippet);
        c->stats.flush_err++;
        return -1;
    }

    c->stats.rows_flushed += (uint64_t)c->pending_rows;
    c->stats.bytes_flushed += (uint64_t)c->batch_len;
    c->stats.flush_ok++;
    c->stats.last_error[0] = '\0';
    c->batch_len = 0;
    c->pending_rows = 0;
    c->table[0] = '\0';
    return 0;
}

int ch_async_insert_json_row(ch_async_client_t *c, const char *table,
                             const char *json_row, size_t json_len)
{
    size_t need;

    if (!c || !json_row || json_len == 0) {
        return -1;
    }
    /* strip trailing whitespace */
    while (json_len > 0 &&
           (json_row[json_len - 1] == '\n' || json_row[json_len - 1] == '\r' ||
            json_row[json_len - 1] == ' ')) {
        json_len--;
    }
    if (json_len == 0 || json_row[0] != '{') {
        snprintf(c->stats.last_error, sizeof(c->stats.last_error),
                 "row must be a JSON object");
        return -1;
    }

    if (table && table[0]) {
        if (c->table[0] && strcmp(c->table, table) != 0) {
            if (ch_async_flush(c) != 0) {
                return -1;
            }
        }
        snprintf(c->table, sizeof(c->table), "%s", table);
    } else if (!c->table[0]) {
        snprintf(c->table, sizeof(c->table), "%s", "e7_netconf_events");
    }

    need = json_len + 1; /* + newline */
    if (c->batch_len + need > c->batch_cap) {
        if (ch_async_flush(c) != 0) {
            return -1;
        }
        if (need > c->batch_cap) {
            snprintf(c->stats.last_error, sizeof(c->stats.last_error),
                     "row too large for batch cap");
            return -1;
        }
        if (table && table[0]) {
            snprintf(c->table, sizeof(c->table), "%s", table);
        }
    }

    memcpy(c->batch + c->batch_len, json_row, json_len);
    c->batch_len += json_len;
    c->batch[c->batch_len++] = '\n';
    c->pending_rows++;
    c->stats.rows_queued++;

    if (c->pending_rows >= c->opt.flush_max_rows ||
        c->batch_len >= c->opt.flush_max_bytes) {
        return ch_async_flush(c);
    }
    return 0;
}

void ch_async_on_tick(ch_async_client_t *c, uint64_t mono_ms)
{
    if (!c || c->pending_rows == 0) {
        return;
    }
    if (mono_ms - c->last_flush_ms >= (uint64_t)c->opt.flush_interval_ms) {
        (void)ch_async_flush(c);
    }
}
