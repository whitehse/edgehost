/**
 * @file edge_http1_serve.c
 * @brief HTTP/1 parse + routing: /health, SPA static, /packages/, 404/400.
 */

#include "edge_http1_serve.h"

#include "edge_static.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http1.h"
#include "protocol_events.h"

struct edge_http1_serve {
    http1_ctx_t        *h1;
    char                method[16];
    char                path[256];
    int                 headers_done;
    int                 done;
    edge_http1_docroot_t roots;
};

static int path_is_health(const char *path)
{
    if (!path) {
        return 0;
    }
    if (strcmp(path, "/health") == 0) {
        return 1;
    }
    if (strncmp(path, "/health?", 8) == 0) {
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
            *suffix_out = path + 9; /* keep leading / for resolve */
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
}

/**
 * Try to serve a static file into out. body starts after headers in out —
 * we build headers after reading body into a temp area at the end of out,
 * or use a stack buffer for body.
 */
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
    body = out + hdr_room; /* temporary: load body at end of buffer */

    if (edge_static_load(root, url_path, body, body_cap, &body_len, ctype,
                         sizeof(ctype)) != 0) {
        return -1;
    }
    /* Rebuild full response at start of out (may overlap — copy body first). */
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

    max_file = s->roots.max_file_bytes ? s->roots.max_file_bytes
                                       : EDGE_STATIC_MAX_FILE;

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

    /* /packages/... → packages_root */
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

    /* SPA root: / and any other path under spa_root */
    if (s->roots.spa_root && s->roots.spa_root[0]) {
        const char *url = s->path[0] ? s->path : "/";
        if (try_static(s->roots.spa_root, url, max_file, out, out_cap,
                       out_len) == 0) {
            note_response(metrics, 200);
            return 1;
        }
        /* SPA fallback: unknown path → index.html (client-side router) */
        if (strcmp(url, "/") != 0 &&
            try_static(s->roots.spa_root, "/index.html", max_file, out, out_cap,
                       out_len) == 0) {
            note_response(metrics, 200);
            return 1;
        }
    } else if (s->path[0] == '\0' || strcmp(s->path, "/") == 0) {
        /* No SPA root configured: keep plain ok for lab */
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
    /* keep roots */
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

int edge_http1_serve_feed(edge_http1_serve_t *s, const uint8_t *data, size_t len,
                          edge_metrics_t *metrics, char *out, size_t out_cap,
                          size_t *out_len)
{
    protocol_event_t ev;
    int bad = 0;
    int respond = 0;

    if (!s || !s->h1 || s->done) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (metrics) {
        metrics->bytes_in += (uint64_t)len;
    }

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
            respond = 1;
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
    if (respond) {
        int rc = dispatch(s, metrics, out, out_cap, out_len);
        if (rc == 1) {
            s->done = 1;
        }
        return rc;
    }
    return 0;
}

const char *edge_http1_serve_method(const edge_http1_serve_t *s)
{
    return s ? s->method : "";
}

const char *edge_http1_serve_path(const edge_http1_serve_t *s)
{
    return s ? s->path : "";
}
