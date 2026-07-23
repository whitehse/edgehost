/**
 * @file edge_clickhouse.c
 * @brief E7 event → ClickHouse + XML→JSON helpers + status.
 */

#include "edge_clickhouse.h"

#include "host_alloc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct edge_clickhouse {
    int                enabled;
    ch_async_client_t *client;
    char               events_table[CH_ASYNC_TABLE_MAX];
    char               database[CH_ASYNC_DB_MAX];
};

edge_clickhouse_t *edge_clickhouse_create(const edge_config_t *cfg)
{
    edge_clickhouse_t *ch;
    ch_async_options_t opt;

    if (!cfg || !cfg->clickhouse_enabled) {
        return NULL;
    }
    ch = (edge_clickhouse_t *)host_alloc_kind(EDGE_MEM_OTHER, sizeof(*ch));
    if (!ch) {
        return NULL;
    }
    ch->enabled = 1;
    ch_async_options_defaults(&opt);
    if (cfg->clickhouse_host[0]) {
        snprintf(opt.host, sizeof(opt.host), "%s", cfg->clickhouse_host);
    }
    if (cfg->clickhouse_port) {
        opt.port = cfg->clickhouse_port;
    }
    if (cfg->clickhouse_database[0]) {
        snprintf(opt.database, sizeof(opt.database), "%s",
                 cfg->clickhouse_database);
    }
    if (cfg->clickhouse_user[0]) {
        snprintf(opt.user, sizeof(opt.user), "%s", cfg->clickhouse_user);
    }
    if (cfg->clickhouse_password[0]) {
        snprintf(opt.password, sizeof(opt.password), "%s",
                 cfg->clickhouse_password);
    }
    if (cfg->clickhouse_base_url[0]) {
        snprintf(opt.base_url, sizeof(opt.base_url), "%s",
                 cfg->clickhouse_base_url);
    }
    opt.use_https = cfg->clickhouse_use_https;
    opt.allow_blocking_dns = cfg->dns_allow_blocking;
    if (cfg->clickhouse_flush_interval_ms) {
        opt.flush_interval_ms = cfg->clickhouse_flush_interval_ms;
    }
    if (cfg->clickhouse_flush_max_rows) {
        opt.flush_max_rows = cfg->clickhouse_flush_max_rows;
    }
    if (cfg->clickhouse_flush_max_bytes) {
        opt.flush_max_bytes = cfg->clickhouse_flush_max_bytes;
    }
    if (cfg->clickhouse_timeout_ms) {
        opt.timeout_ms = cfg->clickhouse_timeout_ms;
    }

    ch->client = ch_async_create(&opt);
    if (!ch->client) {
        host_free(ch);
        return NULL;
    }
    snprintf(ch->database, sizeof(ch->database), "%s",
             opt.database[0] ? opt.database : "edgehost");
    if (cfg->clickhouse_events_table[0]) {
        snprintf(ch->events_table, sizeof(ch->events_table), "%s",
                 cfg->clickhouse_events_table);
    } else {
        snprintf(ch->events_table, sizeof(ch->events_table), "%s.%s",
                 ch->database, "e7_netconf_events");
    }
    return ch;
}

void edge_clickhouse_destroy(edge_clickhouse_t *ch)
{
    if (!ch) {
        return;
    }
    ch_async_destroy(ch->client);
    host_free(ch);
}

int edge_clickhouse_enabled(const edge_clickhouse_t *ch)
{
    return ch && ch->enabled && ch->client;
}

void edge_clickhouse_on_tick(edge_clickhouse_t *ch, uint64_t mono_ms)
{
    if (!edge_clickhouse_enabled(ch)) {
        return;
    }
    ch_async_on_tick(ch->client, mono_ms);
}

int edge_clickhouse_flush(edge_clickhouse_t *ch)
{
    if (!edge_clickhouse_enabled(ch)) {
        return -1;
    }
    return ch_async_flush(ch->client);
}

void edge_clickhouse_stats(const edge_clickhouse_t *ch, ch_async_stats_t *out)
{
    if (!ch || !ch->client) {
        if (out) {
            memset(out, 0, sizeof(*out));
        }
        return;
    }
    ch_async_stats(ch->client, out);
}

int edge_clickhouse_enqueue_json(edge_clickhouse_t *ch, const char *table,
                                 const char *json, size_t json_len)
{
    if (!edge_clickhouse_enabled(ch) || !json || json_len == 0) {
        return -1;
    }
    return ch_async_insert_json_row(
        ch->client, table && table[0] ? table : ch->events_table, json,
        json_len);
}

/* Forward decls used by CPE wrap (defined later with E7 helpers). */
static void iso_now(char *buf, size_t sz);
static int json_esc_field(char *dst, size_t dst_sz, const char *s);

static int json_get_string(const char *json, size_t len, const char *key,
                           char *out, size_t out_sz)
{
    char pat[80];
    const char *p;
    const char *end;
    size_t i = 0;
    int n;

    if (!json || !key || !out || out_sz == 0) {
        return -1;
    }
    out[0] = '\0';
    n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(pat)) {
        return -1;
    }
    end = json + len;
    p = json;
    while (p < end) {
        const char *f = strstr(p, pat);
        if (!f || f >= end) {
            return -1;
        }
        p = f + (size_t)n;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            p++;
        }
        if (p >= end || *p != ':') {
            continue;
        }
        p++;
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (p >= end || *p != '"') {
            /* non-string value — skip */
            return -1;
        }
        p++;
        while (p < end && *p != '"' && i + 1 < out_sz) {
            if (*p == '\\' && p + 1 < end) {
                p++;
            }
            out[i++] = *p++;
        }
        out[i] = '\0';
        return 0;
    }
    return -1;
}

int edge_clickhouse_enqueue_cpe_ndjson(edge_clickhouse_t *ch, const char *json,
                                       size_t json_len)
{
    char type[48];
    char router[80];
    char ts[48];
    char probe[48];
    char event_time[48];
    char nowbuf[48];
    char esc_ev[64];
    char esc_sid[160];
    char esc_et[64];
    char esc_now[64];
    char esc_src[32];
    char esc_probe[64];
    char row[EDGE_CH_JSON_MAX];
    char payload[EDGE_CH_JSON_MAX - 512];
    size_t copy_len;
    size_t i;
    int n;

    if (!edge_clickhouse_enabled(ch) || !json || json_len == 0) {
        return -1;
    }
    /* Already an e7-shaped row? */
    if (strstr(json, "\"event_type\"") && strstr(json, "\"shelf_id\"")) {
        return edge_clickhouse_enqueue_json(ch, NULL, json, json_len);
    }

    type[0] = router[0] = ts[0] = probe[0] = '\0';
    (void)json_get_string(json, json_len, "type", type, sizeof(type));
    (void)json_get_string(json, json_len, "router_id", router, sizeof(router));
    (void)json_get_string(json, json_len, "ts", ts, sizeof(ts));
    (void)json_get_string(json, json_len, "probe", probe, sizeof(probe));

    if (!type[0]) {
        snprintf(type, sizeof(type), "cpe_telemetry");
    }
    if (!router[0]) {
        snprintf(router, sizeof(router), "unknown");
    }

    if (ts[0]) {
        snprintf(event_time, sizeof(event_time), "%s", ts);
        for (i = 0; event_time[i]; i++) {
            if (event_time[i] == 'T') {
                event_time[i] = ' ';
            } else if (event_time[i] == 'Z') {
                event_time[i] = '\0';
                break;
            }
        }
    } else {
        iso_now(event_time, sizeof(event_time));
    }
    iso_now(nowbuf, sizeof(nowbuf));

    if (json_esc_field(esc_ev, sizeof(esc_ev), type) != 0 ||
        json_esc_field(esc_sid, sizeof(esc_sid), router) != 0 ||
        json_esc_field(esc_et, sizeof(esc_et), event_time) != 0 ||
        json_esc_field(esc_now, sizeof(esc_now), nowbuf) != 0 ||
        json_esc_field(esc_src, sizeof(esc_src), "cpe_agent") != 0) {
        return -1;
    }
    if (probe[0]) {
        if (json_esc_field(esc_probe, sizeof(esc_probe), probe) != 0) {
            return -1;
        }
    } else {
        snprintf(esc_probe, sizeof(esc_probe), "\"\"");
    }

    copy_len = json_len;
    while (copy_len > 0 &&
           (json[copy_len - 1] == '\n' || json[copy_len - 1] == '\r' ||
            json[copy_len - 1] == ' ')) {
        copy_len--;
    }
    if (copy_len == 0 || json[0] != '{') {
        return -1;
    }
    if (copy_len >= sizeof(payload)) {
        copy_len = sizeof(payload) - 1;
    }
    memcpy(payload, json, copy_len);
    payload[copy_len] = '\0';

    n = snprintf(row, sizeof(row),
                 "{"
                 "\"event_time\":%s,"
                 "\"ingested_at\":%s,"
                 "\"shelf_id\":%s,"
                 "\"shelf_mac\":\"\","
                 "\"ont_id\":%s,"
                 "\"pon_id\":\"\","
                 "\"event_type\":%s,"
                 "\"severity\":\"info\","
                 "\"source\":%s,"
                 "\"peer\":\"\","
                 "\"payload\":%s,"
                 "\"xml_raw\":\"\""
                 "}",
                 esc_et, esc_now, esc_sid, esc_probe, esc_ev, esc_src, payload);
    if (n < 0 || (size_t)n >= sizeof(row)) {
        return -1;
    }
    return ch_async_insert_json_row(ch->client, ch->events_table, row,
                                    (size_t)n);
}

/* ---- XML helpers ---- */

static int xml_elem_text(const char *xml, size_t len, const char *tag, char *out,
                         size_t out_sz)
{
    char open[96];
    char close[96];
    const char *p;
    const char *end;
    size_t tlen;
    size_t i = 0;

    if (!xml || !tag || !out || out_sz == 0) {
        return -1;
    }
    out[0] = '\0';
    snprintf(open, sizeof(open), "<%s", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    p = xml;
    end = xml + len;
    while (p < end) {
        const char *q = strstr(p, open);
        const char *gt;
        const char *c;
        size_t vlen;
        if (!q || q >= end) {
            return -1;
        }
        gt = strchr(q, '>');
        if (!gt || gt >= end) {
            return -1;
        }
        if (gt > q && gt[-1] == '/') {
            /* empty <tag/> */
            p = gt + 1;
            continue;
        }
        c = strstr(gt + 1, close);
        if (!c || c >= end) {
            return -1;
        }
        vlen = (size_t)(c - (gt + 1));
        while (vlen && isspace((unsigned char)gt[1])) {
            gt++;
            vlen--;
        }
        while (vlen && isspace((unsigned char)gt[vlen])) {
            vlen--;
        }
        if (vlen + 1 > out_sz) {
            vlen = out_sz - 1;
        }
        memcpy(out, gt + 1, vlen);
        out[vlen] = '\0';
        /* strip nested tags crudely */
        for (i = 0; out[i]; i++) {
            if (out[i] == '<') {
                out[i] = '\0';
                break;
            }
        }
        (void)tlen;
        return 0;
    }
    return -1;
}

void edge_e7_event_extract_ids(const char *xml, size_t xml_len, char *ont_id,
                               size_t ont_sz, char *pon_id, size_t pon_sz,
                               char *event_type, size_t type_sz,
                               char *severity, size_t sev_sz,
                               char *event_time, size_t et_sz)
{
    if (ont_id && ont_sz) {
        ont_id[0] = '\0';
    }
    if (pon_id && pon_sz) {
        pon_id[0] = '\0';
    }
    if (event_type && type_sz) {
        event_type[0] = '\0';
    }
    if (severity && sev_sz) {
        severity[0] = '\0';
    }
    if (event_time && et_sz) {
        event_time[0] = '\0';
    }
    if (!xml || xml_len == 0) {
        return;
    }
    if (ont_id && ont_sz) {
        (void)xml_elem_text(xml, xml_len, "ont-id", ont_id, ont_sz);
        if (!ont_id[0]) {
            (void)xml_elem_text(xml, xml_len, "ont_id", ont_id, ont_sz);
        }
    }
    if (pon_id && pon_sz) {
        (void)xml_elem_text(xml, xml_len, "pon-id", pon_id, pon_sz);
        if (!pon_id[0]) {
            (void)xml_elem_text(xml, xml_len, "pon_id", pon_id, pon_sz);
        }
    }
    if (severity && sev_sz) {
        (void)xml_elem_text(xml, xml_len, "severity", severity, sev_sz);
    }
    if (event_time && et_sz) {
        (void)xml_elem_text(xml, xml_len, "eventTime", event_time, et_sz);
        if (!event_time[0]) {
            (void)xml_elem_text(xml, xml_len, "event-time", event_time, et_sz);
        }
    }
    if (event_type && type_sz) {
        if (strstr(xml, "ont-event") || strstr(xml, "<ont-event")) {
            snprintf(event_type, type_sz, "%s", "ont-event");
        } else if (strstr(xml, "pon-alarm") || strstr(xml, "<pon-alarm")) {
            snprintf(event_type, type_sz, "%s", "pon-alarm");
        } else {
            char name[64];
            if (xml_elem_text(xml, xml_len, "name", name, sizeof(name)) == 0 &&
                name[0]) {
                snprintf(event_type, type_sz, "%s", name);
            } else {
                snprintf(event_type, type_sz, "%s", "notification");
            }
        }
    }
}

static int json_escape_append(char *out, size_t out_sz, size_t *off,
                              const char *s, size_t slen)
{
    size_t i;
    if (*off + 1 >= out_sz) {
        return -1;
    }
    out[(*off)++] = '"';
    for (i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            if (*off + 2 >= out_sz) {
                return -1;
            }
            out[(*off)++] = '\\';
            out[(*off)++] = (char)c;
        } else if (c < 0x20) {
            continue;
        } else {
            if (*off + 1 >= out_sz) {
                return -1;
            }
            out[(*off)++] = (char)c;
        }
    }
    if (*off + 1 >= out_sz) {
        return -1;
    }
    out[(*off)++] = '"';
    return 0;
}

/**
 * Minimal XML→JSON: walk top-level-ish elements under root.
 * Strategy: find each <tag>text</tag> or nested blocks; build flat-ish object
 * with nested objects for elements containing children.
 */
int edge_xml_to_json(const char *xml, size_t xml_len, char *out, size_t out_sz)
{
    size_t off = 0;
    const char *p;
    const char *end;
    int first = 1;
    int depth = 0;

    if (!xml || !out || out_sz < 3) {
        return -1;
    }
    /* skip to first element after notification/rpc wrappers if present */
    p = xml;
    end = xml + xml_len;
    while (p < end && *p != '<') {
        p++;
    }
    if (p >= end) {
        snprintf(out, out_sz, "{}");
        return 2;
    }

    if (off + 1 >= out_sz) {
        return -1;
    }
    out[off++] = '{';

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        const char *gt;
        const char *name_end;
        char tag[64];
        size_t tlen;
        int self_close;
        int is_close;

        if (!lt) {
            break;
        }
        if (lt + 1 >= end) {
            break;
        }
        if (lt[1] == '?' || lt[1] == '!') {
            p = lt + 2;
            continue;
        }
        is_close = (lt[1] == '/');
        name_end = is_close ? lt + 2 : lt + 1;
        tlen = 0;
        while (name_end < end && tlen + 1 < sizeof(tag)) {
            char c = *name_end;
            if (c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\n' ||
                c == '\r') {
                break;
            }
            /* strip namespace prefix */
            if (c == ':') {
                tlen = 0;
                name_end++;
                continue;
            }
            tag[tlen++] = c;
            name_end++;
        }
        tag[tlen] = '\0';
        gt = memchr(name_end, '>', (size_t)(end - name_end));
        if (!gt) {
            break;
        }
        self_close = (gt > name_end && gt[-1] == '/');

        if (is_close) {
            if (depth > 0) {
                depth--;
            }
            p = gt + 1;
            continue;
        }
        if (self_close) {
            if (!first) {
                if (off + 1 >= out_sz) {
                    return -1;
                }
                out[off++] = ',';
            }
            first = 0;
            if (json_escape_append(out, out_sz, &off, tag, tlen) != 0) {
                return -1;
            }
            if (off + 6 >= out_sz) {
                return -1;
            }
            memcpy(out + off, ":null", 5);
            off += 5;
            p = gt + 1;
            continue;
        }

        {
            char close[80];
            const char *cpos;
            size_t vlen;
            int nested;
            snprintf(close, sizeof(close), "</%s>", tag);
            /* also try without ns — close tag may include prefix; search
             * loosely */
            cpos = strstr(gt + 1, close);
            if (!cpos) {
                /* find </tag with optional prefix */
                const char *q = gt + 1;
                while (q < end) {
                    const char *s = strstr(q, "</");
                    const char *e;
                    if (!s) {
                        break;
                    }
                    e = strchr(s, '>');
                    if (!e) {
                        break;
                    }
                    if (e - s >= 3) {
                        const char *tn = e;
                        while (tn > s + 2 && tn[-1] != ':' && tn[-1] != '/') {
                            tn--;
                        }
                        if ((size_t)(e - tn) == tlen &&
                            strncmp(tn, tag, tlen) == 0) {
                            cpos = s;
                            close[0] = '\0';
                            break;
                        }
                    }
                    q = s + 2;
                }
            }
            if (!cpos || cpos >= end) {
                p = gt + 1;
                continue;
            }
            vlen = (size_t)(cpos - (gt + 1));
            nested = (memchr(gt + 1, '<', vlen) != NULL);

            if (!first) {
                if (off + 1 >= out_sz) {
                    return -1;
                }
                out[off++] = ',';
            }
            first = 0;
            if (json_escape_append(out, out_sz, &off, tag, tlen) != 0) {
                return -1;
            }
            if (off + 1 >= out_sz) {
                return -1;
            }
            out[off++] = ':';

            if (nested) {
                char nested_json[EDGE_CH_JSON_MAX / 2];
                int nj = edge_xml_to_json(gt + 1, vlen, nested_json,
                                          sizeof(nested_json));
                if (nj < 0) {
                    if (json_escape_append(out, out_sz, &off, gt + 1, vlen) !=
                        0) {
                        return -1;
                    }
                } else {
                    if (off + (size_t)nj >= out_sz) {
                        return -1;
                    }
                    memcpy(out + off, nested_json, (size_t)nj);
                    off += (size_t)nj;
                }
            } else {
                /* trim whitespace */
                const char *vs = gt + 1;
                while (vlen && isspace((unsigned char)*vs)) {
                    vs++;
                    vlen--;
                }
                while (vlen && isspace((unsigned char)vs[vlen - 1])) {
                    vlen--;
                }
                if (json_escape_append(out, out_sz, &off, vs, vlen) != 0) {
                    return -1;
                }
            }
            /* advance past close */
            {
                const char *ce = strchr(cpos, '>');
                p = ce ? ce + 1 : cpos + 1;
            }
        }
    }

    if (off + 1 >= out_sz) {
        return -1;
    }
    out[off++] = '}';
    out[off] = '\0';
    return (int)off;
}

static void iso_now(char *buf, size_t sz)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S.000", &tm);
}

static int json_esc_field(char *dst, size_t dst_sz, const char *s)
{
    size_t o = 0;
    size_t i;
    if (!s) {
        s = "";
    }
    if (o + 1 >= dst_sz) {
        return -1;
    }
    dst[o++] = '"';
    for (i = 0; s[i] && o + 2 < dst_sz; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            continue;
        } else {
            dst[o++] = (char)c;
        }
    }
    if (o + 1 >= dst_sz) {
        return -1;
    }
    dst[o++] = '"';
    dst[o] = '\0';
    return 0;
}

int edge_clickhouse_enqueue_e7_event(edge_clickhouse_t *ch,
                                     const char *shelf_mac,
                                     const char *shelf_id, const char *peer,
                                     const char *notification_xml,
                                     size_t xml_len, const char *source)
{
    char ont_id[64], pon_id[64], event_type[64], severity[32], event_time[48];
    char payload_json[EDGE_CH_JSON_MAX];
    char row[EDGE_CH_JSON_MAX + 1024];
    char esc_mac[80], esc_sid[160], esc_peer[96], esc_ont[96], esc_pon[96];
    char esc_type[96], esc_sev[48], esc_src[48], esc_et[64];
    char esc_xml[EDGE_CH_JSON_MAX];
    char nowbuf[40];
    size_t xml_copy;
    size_t i, o;
    int n;
    int pj;

    if (!edge_clickhouse_enabled(ch) || !notification_xml || xml_len == 0) {
        return -1;
    }

    edge_e7_event_extract_ids(notification_xml, xml_len, ont_id, sizeof(ont_id),
                              pon_id, sizeof(pon_id), event_type,
                              sizeof(event_type), severity, sizeof(severity),
                              event_time, sizeof(event_time));
    pj = edge_xml_to_json(notification_xml, xml_len, payload_json,
                          sizeof(payload_json));
    if (pj < 0) {
        snprintf(payload_json, sizeof(payload_json), "{}");
    }

    if (!event_time[0]) {
        iso_now(event_time, sizeof(event_time));
    } else {
        /* ClickHouse DateTime64 prefers 'YYYY-MM-DD HH:MM:SS' — replace T */
        for (i = 0; event_time[i]; i++) {
            if (event_time[i] == 'T') {
                event_time[i] = ' ';
            }
            if (event_time[i] == 'Z') {
                event_time[i] = '\0';
                break;
            }
        }
    }
    iso_now(nowbuf, sizeof(nowbuf));

    if (json_esc_field(esc_mac, sizeof(esc_mac),
                       shelf_mac ? shelf_mac : "") != 0 ||
        json_esc_field(esc_sid, sizeof(esc_sid),
                       shelf_id && shelf_id[0] ? shelf_id
                                               : (shelf_mac ? shelf_mac : "")) !=
            0 ||
        json_esc_field(esc_peer, sizeof(esc_peer), peer ? peer : "") != 0 ||
        json_esc_field(esc_ont, sizeof(esc_ont), ont_id) != 0 ||
        json_esc_field(esc_pon, sizeof(esc_pon), pon_id) != 0 ||
        json_esc_field(esc_type, sizeof(esc_type), event_type) != 0 ||
        json_esc_field(esc_sev, sizeof(esc_sev), severity) != 0 ||
        json_esc_field(esc_src, sizeof(esc_src),
                       source && source[0] ? source : "e7") != 0 ||
        json_esc_field(esc_et, sizeof(esc_et), event_time) != 0) {
        return -1;
    }

    /* escape xml_raw as JSON string */
    o = 0;
    esc_xml[o++] = '"';
    xml_copy = xml_len < 12000 ? xml_len : 12000;
    for (i = 0; i < xml_copy && o + 2 < sizeof(esc_xml) - 1; i++) {
        unsigned char c = (unsigned char)notification_xml[i];
        if (c == '"' || c == '\\') {
            esc_xml[o++] = '\\';
            esc_xml[o++] = (char)c;
        } else if (c < 0x20) {
            continue;
        } else {
            esc_xml[o++] = (char)c;
        }
    }
    esc_xml[o++] = '"';
    esc_xml[o] = '\0';

    /* payload is embedded as JSON object (JSONEachRow accepts nested object) */
    n = snprintf(row, sizeof(row),
                 "{"
                 "\"event_time\":%s,"
                 "\"ingested_at\":%s,"
                 "\"shelf_id\":%s,"
                 "\"shelf_mac\":%s,"
                 "\"ont_id\":%s,"
                 "\"pon_id\":%s,"
                 "\"event_type\":%s,"
                 "\"severity\":%s,"
                 "\"source\":%s,"
                 "\"peer\":%s,"
                 "\"payload\":%s,"
                 "\"xml_raw\":%s"
                 "}",
                 esc_et, esc_et /* ingested replaced below */, esc_sid, esc_mac,
                 esc_ont, esc_pon, esc_type, esc_sev, esc_src, esc_peer,
                 payload_json, esc_xml);
    /* fix ingested_at to now */
    {
        char esc_now[64];
        if (json_esc_field(esc_now, sizeof(esc_now), nowbuf) != 0) {
            return -1;
        }
        n = snprintf(row, sizeof(row),
                     "{"
                     "\"event_time\":%s,"
                     "\"ingested_at\":%s,"
                     "\"shelf_id\":%s,"
                     "\"shelf_mac\":%s,"
                     "\"ont_id\":%s,"
                     "\"pon_id\":%s,"
                     "\"event_type\":%s,"
                     "\"severity\":%s,"
                     "\"source\":%s,"
                     "\"peer\":%s,"
                     "\"payload\":%s,"
                     "\"xml_raw\":%s"
                     "}",
                     esc_et, esc_now, esc_sid, esc_mac, esc_ont, esc_pon,
                     esc_type, esc_sev, esc_src, esc_peer, payload_json,
                     esc_xml);
    }
    if (n < 0 || (size_t)n >= sizeof(row)) {
        return -1;
    }
    return ch_async_insert_json_row(ch->client, ch->events_table, row,
                                    (size_t)n);
}

int edge_clickhouse_status_json(const edge_clickhouse_t *ch, char *buf,
                                size_t buf_sz)
{
    ch_async_stats_t st;
    int n;

    if (!buf || buf_sz < 32) {
        return -1;
    }
    if (!edge_clickhouse_enabled(ch)) {
        n = snprintf(buf, buf_sz, "{\"enabled\":false}");
        return (n < 0 || (size_t)n >= buf_sz) ? -1 : n;
    }
    ch_async_stats(ch->client, &st);
    n = snprintf(buf, buf_sz,
                 "{"
                 "\"enabled\":true,"
                 "\"table\":\"%s\","
                 "\"pending_rows\":%zu,"
                 "\"rows_queued\":%llu,"
                 "\"rows_flushed\":%llu,"
                 "\"flush_ok\":%llu,"
                 "\"flush_err\":%llu,"
                 "\"bytes_flushed\":%llu,"
                 "\"last_http_status\":%llu,"
                 "\"last_error\":%s%s%s"
                 "}",
                 ch->events_table, ch_async_pending_rows(ch->client),
                 (unsigned long long)st.rows_queued,
                 (unsigned long long)st.rows_flushed,
                 (unsigned long long)st.flush_ok,
                 (unsigned long long)st.flush_err,
                 (unsigned long long)st.bytes_flushed,
                 (unsigned long long)st.last_http_status,
                 st.last_error[0] ? "\"" : "null",
                 st.last_error[0] ? st.last_error : "",
                 st.last_error[0] ? "\"" : "");
    if (n < 0 || (size_t)n >= buf_sz) {
        return -1;
    }
    return n;
}
