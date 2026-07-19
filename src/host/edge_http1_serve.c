/**
 * @file edge_http1_serve.c
 * @brief Shared HTTP/1 parse + routing (health JSON / root ok / 404 / 400).
 */

#include "edge_http1_serve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http1.h"
#include "protocol_events.h"

struct edge_http1_serve {
    http1_ctx_t *h1;
    char         method[16];
    char         path[256];
    int          headers_done;
    int          done; /* already produced a response */
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

static int dispatch(edge_http1_serve_t *s, edge_metrics_t *metrics, char *out,
                    size_t out_cap, size_t *out_len)
{
    char json[512];
    int jn;
    static const char ok_body[] = "ok\n";
    static const char bad_body[] = "bad request\n";
    static const char nf_body[] = "not found\n";
    static const char ma_body[] = "method not allowed\n";

    (void)bad_body;

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
    if (s->path[0] == '\0' || strcmp(s->path, "/") == 0) {
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
