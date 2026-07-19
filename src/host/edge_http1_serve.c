/**
 * @file edge_http1_serve.c
 * @brief HTTP/1 parse + routing: health, SPA, packages, state REST (P1.7a).
 */

#include "edge_http1_serve.h"

#include "edge_static.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http1.h"
#include "protocol_events.h"

#define EDGE_HTTP_ACC_MAX (EDGE_STATE_VALUE_MAX + 4096)

struct edge_http1_serve {
    http1_ctx_t         *h1;
    char                 method[16];
    char                 path[512];
    int                  headers_done;
    int                  done;
    edge_http1_docroot_t roots;
    edge_state_store_t  *store; /* not owned */

    /* Accumulated request for body (PUT) — host buffer */
    uint8_t              acc[EDGE_HTTP_ACC_MAX];
    size_t               acc_len;
    long                 content_length; /* -1 unknown */
    size_t               body_off;       /* offset of body in acc after headers */
    int                  have_body_off;
};

static int path_is_health(const char *path)
{
    if (!path) {
        return 0;
    }
    if (strcmp(path, "/health") == 0 || strncmp(path, "/health?", 8) == 0) {
        return 1;
    }
    return 0;
}

static int path_is_packages(const char *path, const char **suffix_out)
{
    if (!path) {
        return 0;
    }
    if (strncmp(path, "/packages/", 10) == 0) {
        if (suffix_out) {
            *suffix_out = path + 9;
        }
        return 1;
    }
    if (strcmp(path, "/packages") == 0) {
        if (suffix_out) {
            *suffix_out = "/";
        }
        return 1;
    }
    return 0;
}

/** Parse /api/v1/state/{ns} or /api/v1/state/{ns}/{key...} */
static int path_is_state(const char *path, char *ns_out, size_t ns_sz,
                         char *key_out, size_t key_sz, char *prefix_out,
                         size_t prefix_sz, int *is_list)
{
    const char *p;
    const char *q;
    const char *slash;
    size_t nlen;

    if (is_list) {
        *is_list = 0;
    }
    if (prefix_out && prefix_sz) {
        prefix_out[0] = '\0';
    }
    if (key_out && key_sz) {
        key_out[0] = '\0';
    }
    if (!path || strncmp(path, "/api/v1/state/", 14) != 0) {
        return 0;
    }
    p = path + 14;
    /* strip query for ns/key split */
    q = strchr(p, '?');
    {
        char tmp[512];
        size_t plen = q ? (size_t)(q - p) : strlen(p);
        if (plen >= sizeof(tmp)) {
            return 0;
        }
        memcpy(tmp, p, plen);
        tmp[plen] = '\0';
        p = tmp;

        slash = strchr(p, '/');
        if (!slash) {
            /* /api/v1/state/{ns}  list */
            nlen = strlen(p);
            if (nlen == 0 || nlen >= ns_sz) {
                return 0;
            }
            memcpy(ns_out, p, nlen + 1);
            if (is_list) {
                *is_list = 1;
            }
            if (q && prefix_out && prefix_sz) {
                const char *pr = strstr(q, "prefix=");
                if (pr) {
                    pr += 7;
                    {
                        size_t i = 0;
                        while (pr[i] && pr[i] != '&' && i + 1 < prefix_sz) {
                            prefix_out[i] = pr[i];
                            i++;
                        }
                        prefix_out[i] = '\0';
                    }
                }
            }
            return 1;
        }
        nlen = (size_t)(slash - p);
        if (nlen == 0 || nlen >= ns_sz) {
            return 0;
        }
        memcpy(ns_out, p, nlen);
        ns_out[nlen] = '\0';
        /* key is rest */
        {
            const char *k = slash + 1;
            size_t klen = strlen(k);
            if (klen == 0 || klen >= key_sz) {
                return 0;
            }
            memcpy(key_out, k, klen + 1);
        }
        return 1;
    }
}

static int build_response(char *out, size_t out_cap, int status,
                          const char *reason, const char *ctype,
                          const char *body, size_t body_len, size_t *out_len)
{
    int n;

    if (!out || out_cap < 64 || !out_len) {
        return -1;
    }
    if (!reason) {
        reason = "OK";
    }
    if (!ctype) {
        ctype = "text/plain";
    }
    if (!body) {
        body = "";
        body_len = 0;
    }
    n = snprintf(out, out_cap,
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 status, reason, ctype, body_len);
    if (n < 0 || (size_t)n >= out_cap) {
        return -1;
    }
    if (body_len > 0) {
        if ((size_t)n + body_len >= out_cap) {
            return -1;
        }
        memcpy(out + n, body, body_len);
        n += (int)body_len;
    }
    *out_len = (size_t)n;
    return 0;
}

static void note_response(edge_metrics_t *m, int status)
{
    if (!m) {
        return;
    }
    m->requests++;
    if (status >= 200 && status < 300) {
        m->responses_2xx++;
    } else if (status >= 400 && status < 500) {
        m->responses_4xx++;
    }
    /* 204 counted as 2xx above */
}

static int err_to_http(edge_state_err_t e, int *status, const char **reason,
                       char *body, size_t body_sz)
{
    switch (e) {
    case EDGE_STATE_OK:
        *status = 200;
        *reason = "OK";
        snprintf(body, body_sz, "{\"ok\":true}");
        return 0;
    case EDGE_STATE_NOT_FOUND:
        *status = 404;
        *reason = "Not Found";
        snprintf(body, body_sz, "{\"error\":\"NOT_FOUND\"}");
        return 0;
    case EDGE_STATE_NS_DISABLED:
        *status = 403;
        *reason = "Forbidden";
        snprintf(body, body_sz, "{\"error\":\"NS_DISABLED\"}");
        return 0;
    case EDGE_STATE_TOO_LARGE:
        *status = 413;
        *reason = "Payload Too Large";
        snprintf(body, body_sz, "{\"error\":\"TOO_LARGE\"}");
        return 0;
    case EDGE_STATE_NS_FULL:
        *status = 507;
        *reason = "Insufficient Storage";
        snprintf(body, body_sz, "{\"error\":\"NS_FULL\"}");
        return 0;
    case EDGE_STATE_BAD_KEY:
    case EDGE_STATE_BAD_JSON:
    case EDGE_STATE_BAD_NS:
        *status = 400;
        *reason = "Bad Request";
        snprintf(body, body_sz, "{\"error\":\"%s\"}", edge_state_err_name(e));
        return 0;
    default:
        *status = 500;
        *reason = "Internal Server Error";
        snprintf(body, body_sz, "{\"error\":\"INTERNAL\"}");
        return 0;
    }
}

static int try_static(const char *root, const char *url_path, size_t max_file,
                      char *out, size_t out_cap, size_t *out_len)
{
    char *body;
    size_t body_cap;
    size_t body_len = 0;
    char ctype[96];
    size_t hdr_room = 256;

    if (!root || !root[0] || !out || out_cap <= hdr_room + 1) {
        return -1;
    }
    if (max_file == 0) {
        max_file = EDGE_STATIC_MAX_FILE;
    }
    body_cap = out_cap - hdr_room;
    if (body_cap > max_file) {
        body_cap = max_file;
    }
    body = out + hdr_room;
    if (edge_static_load(root, url_path, body, body_cap, &body_len, ctype,
                         sizeof(ctype)) != 0) {
        return -1;
    }
    {
        char *tmp = (char *)malloc(body_len ? body_len : 1);
        if (!tmp && body_len > 0) {
            return -1;
        }
        if (body_len > 0) {
            memcpy(tmp, body, body_len);
        }
        if (build_response(out, out_cap, 200, "OK", ctype, tmp, body_len,
                           out_len) != 0) {
            free(tmp);
            return -1;
        }
        free(tmp);
    }
    return 0;
}

static int dispatch_state(edge_http1_serve_t *s, edge_metrics_t *metrics,
                          char *out, size_t out_cap, size_t *out_len)
{
    char ns[64];
    char key[EDGE_STATE_KEY_MAX];
    char prefix[EDGE_STATE_KEY_MAX];
    int is_list = 0;
    edge_state_err_t er;
    int status;
    const char *reason;
    char errbody[128];
    char valbuf[EDGE_STATE_VALUE_MAX + 1];
    size_t vlen = 0;

    if (!path_is_state(s->path, ns, sizeof(ns), key, sizeof(key), prefix,
                       sizeof(prefix), &is_list)) {
        return 0; /* not state path */
    }
    if (!s->store) {
        if (build_response(out, out_cap, 503, "Service Unavailable",
                           "application/json",
                           "{\"error\":\"STATE_UNAVAILABLE\"}", 30,
                           out_len) != 0) {
            return -1;
        }
        note_response(metrics, 503);
        return 1;
    }

    if (strcmp(s->method, "GET") == 0) {
        if (is_list) {
            char keys[64][EDGE_STATE_KEY_MAX];
            int n, i;
            size_t off = 0;
            char listbuf[8192];

            n = edge_state_list(s->store, ns, prefix[0] ? prefix : NULL, keys,
                                64);
            if (n < 0) {
                er = edge_state_ns_enabled(s->store, ns) ? EDGE_STATE_BAD_NS
                                                         : EDGE_STATE_NS_DISABLED;
                err_to_http(er, &status, &reason, errbody, sizeof(errbody));
                if (build_response(out, out_cap, status, reason,
                                   "application/json", errbody, strlen(errbody),
                                   out_len) != 0) {
                    return -1;
                }
                note_response(metrics, status);
                return 1;
            }
            off = (size_t)snprintf(listbuf, sizeof(listbuf), "{\"keys\":[");
            for (i = 0; i < n && off + EDGE_STATE_KEY_MAX + 4 < sizeof(listbuf);
                 i++) {
                int w = snprintf(listbuf + off, sizeof(listbuf) - off, "%s\"%s\"",
                                 i ? "," : "", keys[i]);
                if (w < 0) {
                    break;
                }
                off += (size_t)w;
            }
            if (off + 2 < sizeof(listbuf)) {
                listbuf[off++] = ']';
                listbuf[off++] = '}';
                listbuf[off] = '\0';
            }
            if (build_response(out, out_cap, 200, "OK", "application/json",
                               listbuf, off, out_len) != 0) {
                return -1;
            }
            note_response(metrics, 200);
            return 1;
        }
        er = edge_state_get(s->store, ns, key, valbuf, sizeof(valbuf), &vlen);
        if (er != EDGE_STATE_OK) {
            err_to_http(er, &status, &reason, errbody, sizeof(errbody));
            if (build_response(out, out_cap, status, reason, "application/json",
                               errbody, strlen(errbody), out_len) != 0) {
                return -1;
            }
            note_response(metrics, status);
            return 1;
        }
        if (build_response(out, out_cap, 200, "OK", "application/json", valbuf,
                           vlen, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 200);
        return 1;
    }

    if (strcmp(s->method, "PUT") == 0) {
        const char *body;
        size_t blen;
        if (is_list || !key[0]) {
            if (build_response(out, out_cap, 400, "Bad Request",
                               "application/json",
                               "{\"error\":\"BAD_KEY\"}", 18, out_len) != 0) {
                return -1;
            }
            note_response(metrics, 400);
            return 1;
        }
        if (!s->have_body_off) {
            /* need more body */
            return 0;
        }
        body = (const char *)(s->acc + s->body_off);
        blen = s->acc_len > s->body_off ? s->acc_len - s->body_off : 0;
        if (s->content_length >= 0 && blen < (size_t)s->content_length) {
            return 0; /* wait for more */
        }
        if (s->content_length >= 0 && blen > (size_t)s->content_length) {
            blen = (size_t)s->content_length;
        }
        er = edge_state_put(s->store, ns, key, body, blen);
        err_to_http(er, &status, &reason, errbody, sizeof(errbody));
        if (er == EDGE_STATE_OK) {
            status = 204;
            reason = "No Content";
            if (build_response(out, out_cap, status, reason, "application/json",
                               "", 0, out_len) != 0) {
                return -1;
            }
        } else {
            if (build_response(out, out_cap, status, reason, "application/json",
                               errbody, strlen(errbody), out_len) != 0) {
                return -1;
            }
        }
        note_response(metrics, status);
        return 1;
    }

    if (strcmp(s->method, "DELETE") == 0) {
        if (is_list || !key[0]) {
            if (build_response(out, out_cap, 400, "Bad Request",
                               "application/json",
                               "{\"error\":\"BAD_KEY\"}", 18, out_len) != 0) {
                return -1;
            }
            note_response(metrics, 400);
            return 1;
        }
        er = edge_state_delete(s->store, ns, key);
        if (er == EDGE_STATE_OK) {
            if (build_response(out, out_cap, 204, "No Content",
                               "application/json", "", 0, out_len) != 0) {
                return -1;
            }
            note_response(metrics, 204);
            return 1;
        }
        err_to_http(er, &status, &reason, errbody, sizeof(errbody));
        if (build_response(out, out_cap, status, reason, "application/json",
                           errbody, strlen(errbody), out_len) != 0) {
            return -1;
        }
        note_response(metrics, status);
        return 1;
    }

    if (build_response(out, out_cap, 405, "Method Not Allowed",
                       "application/json",
                       "{\"error\":\"METHOD\"}", 18, out_len) != 0) {
        return -1;
    }
    note_response(metrics, 405);
    return 1;
}

static int dispatch(edge_http1_serve_t *s, edge_metrics_t *metrics, char *out,
                    size_t out_cap, size_t *out_len)
{
    char json[512];
    int jn;
    static const char ok_body[] = "ok\n";
    static const char nf_body[] = "not found\n";
    static const char ma_body[] = "method not allowed\n";
    size_t max_file;
    const char *pkg_suffix = NULL;
    int st;

    max_file = s->roots.max_file_bytes ? s->roots.max_file_bytes
                                       : EDGE_STATIC_MAX_FILE;

    /* State API first (supports GET/PUT/DELETE) */
    if (strncmp(s->path, "/api/v1/state/", 14) == 0) {
        st = dispatch_state(s, metrics, out, out_cap, out_len);
        if (st != 0) {
            return st;
        }
    }

    if (s->method[0] && strcmp(s->method, "GET") != 0) {
        if (build_response(out, out_cap, 405, "Method Not Allowed", "text/plain",
                           ma_body, sizeof(ma_body) - 1, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 405);
        return 1;
    }

    if (path_is_health(s->path)) {
        if (!metrics) {
            edge_metrics_t tmp;
            edge_metrics_init(&tmp);
            jn = edge_metrics_format_health_json(&tmp, json, sizeof(json));
        } else {
            jn = edge_metrics_format_health_json(metrics, json, sizeof(json));
        }
        if (jn < 0) {
            if (build_response(out, out_cap, 500, "Internal Server Error",
                               "text/plain", "metrics error\n", 14,
                               out_len) != 0) {
                return -1;
            }
            note_response(metrics, 500);
            return 1;
        }
        if (build_response(out, out_cap, 200, "OK", "application/json", json,
                           (size_t)jn, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 200);
        return 1;
    }

    if (path_is_packages(s->path, &pkg_suffix)) {
        if (s->roots.packages_root && s->roots.packages_root[0] &&
            try_static(s->roots.packages_root, pkg_suffix, max_file, out,
                       out_cap, out_len) == 0) {
            note_response(metrics, 200);
            return 1;
        }
        if (build_response(out, out_cap, 404, "Not Found", "text/plain",
                           nf_body, sizeof(nf_body) - 1, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 404);
        return 1;
    }

    if (s->roots.spa_root && s->roots.spa_root[0]) {
        const char *url = s->path[0] ? s->path : "/";
        if (try_static(s->roots.spa_root, url, max_file, out, out_cap,
                       out_len) == 0) {
            note_response(metrics, 200);
            return 1;
        }
        if (strcmp(url, "/") != 0 &&
            try_static(s->roots.spa_root, "/index.html", max_file, out, out_cap,
                       out_len) == 0) {
            note_response(metrics, 200);
            return 1;
        }
    } else if (s->path[0] == '\0' || strcmp(s->path, "/") == 0) {
        if (build_response(out, out_cap, 200, "OK", "text/plain", ok_body,
                           sizeof(ok_body) - 1, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 200);
        return 1;
    }

    if (build_response(out, out_cap, 404, "Not Found", "text/plain", nf_body,
                       sizeof(nf_body) - 1, out_len) != 0) {
        return -1;
    }
    note_response(metrics, 404);
    return 1;
}

edge_http1_serve_t *edge_http1_serve_create(void)
{
    edge_http1_serve_t *s = (edge_http1_serve_t *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->h1 = http1_create(HTTP1_ROLE_SERVER);
    if (!s->h1) {
        free(s);
        return NULL;
    }
    s->content_length = -1;
    return s;
}

void edge_http1_serve_destroy(edge_http1_serve_t *s)
{
    if (!s) {
        return;
    }
    if (s->h1) {
        http1_destroy(s->h1);
    }
    free(s);
}

void edge_http1_serve_reset(edge_http1_serve_t *s)
{
    if (!s) {
        return;
    }
    if (s->h1) {
        http1_reset(s->h1);
    }
    s->method[0] = s->path[0] = '\0';
    s->headers_done = 0;
    s->done = 0;
    s->acc_len = 0;
    s->content_length = -1;
    s->body_off = 0;
    s->have_body_off = 0;
}

void edge_http1_serve_set_docroots(edge_http1_serve_t *s,
                                   const edge_http1_docroot_t *roots)
{
    if (!s) {
        return;
    }
    if (!roots) {
        memset(&s->roots, 0, sizeof(s->roots));
        return;
    }
    s->roots = *roots;
}

void edge_http1_serve_set_state(edge_http1_serve_t *s, edge_state_store_t *st)
{
    if (s) {
        s->store = st;
    }
}

static void mark_body_off(edge_http1_serve_t *s)
{
    /* Find end of headers in acc */
    size_t i;
    for (i = 0; i + 3 < s->acc_len; i++) {
        if (s->acc[i] == '\r' && s->acc[i + 1] == '\n' && s->acc[i + 2] == '\r' &&
            s->acc[i + 3] == '\n') {
            s->body_off = i + 4;
            s->have_body_off = 1;
            return;
        }
        if (s->acc[i] == '\n' && s->acc[i + 1] == '\n') {
            s->body_off = i + 2;
            s->have_body_off = 1;
            return;
        }
    }
}

int edge_http1_serve_feed(edge_http1_serve_t *s, const uint8_t *data, size_t len,
                          edge_metrics_t *metrics, char *out, size_t out_cap,
                          size_t *out_len)
{
    protocol_event_t ev;
    int bad = 0;
    int headers_just_done = 0;
    char clbuf[32];

    if (!s || !s->h1 || s->done) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (metrics) {
        metrics->bytes_in += (uint64_t)len;
    }

    /* Accumulate for body extraction */
    if (s->acc_len + len > sizeof(s->acc)) {
        static const char body[] = "request too large\n";
        if (build_response(out, out_cap, 413, "Payload Too Large", "text/plain",
                           body, sizeof(body) - 1, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 413);
        s->done = 1;
        return 1;
    }
    memcpy(s->acc + s->acc_len, data, len);
    s->acc_len += len;

    (void)http1_feed_input(s->h1, data, len);

    while (http1_next_event(s->h1, &ev)) {
        if (ev.type == PROTOCOL_EVENT_ERROR) {
            bad = 1;
            break;
        }
        if (ev.type == HTTP1_EVENT_REQUEST_LINE) {
            snprintf(s->method, sizeof(s->method), "%s",
                     ev.u.http1_request.method);
            snprintf(s->path, sizeof(s->path), "%s",
                     ev.u.http1_request.path);
        } else if (ev.type == HTTP1_EVENT_HEADERS_COMPLETE) {
            s->headers_done = 1;
            headers_just_done = 1;
            mark_body_off(s);
            if (http1_get_header(s->h1, "Content-Length", clbuf,
                                 sizeof(clbuf)) == 0) {
                s->content_length = atol(clbuf);
            } else {
                s->content_length = 0;
            }
        } else if (ev.type == HTTP1_EVENT_BODY_CHUNK) {
            /* body also in acc; length tracked via content_length */
        }
    }

    if (bad) {
        static const char body[] = "bad request\n";
        if (build_response(out, out_cap, 400, "Bad Request", "text/plain", body,
                           sizeof(body) - 1, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 400);
        s->done = 1;
        return 1;
    }

    if (!s->headers_done) {
        return 0;
    }

    /* PUT with body: wait until full body present in acc */
    if (strcmp(s->method, "PUT") == 0 && s->content_length > 0) {
        size_t have = 0;
        if (!s->have_body_off) {
            mark_body_off(s);
        }
        if (s->have_body_off) {
            have = s->acc_len - s->body_off;
        }
        if (have < (size_t)s->content_length) {
            (void)headers_just_done;
            return 0;
        }
    }

    {
        int rc = dispatch(s, metrics, out, out_cap, out_len);
        if (rc == 1) {
            s->done = 1;
        }
        return rc;
    }
}

const char *edge_http1_serve_method(const edge_http1_serve_t *s)
{
    return s ? s->method : "";
}

const char *edge_http1_serve_path(const edge_http1_serve_t *s)
{
    return s ? s->path : "";
}
