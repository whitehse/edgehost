/**
 * @file edge_http1_serve.c
 * @brief HTTP/1 parse + routing: health, SPA, packages, state, WS, E7, lab auth.
 */

#include "edge_http1_serve.h"

#include "edge_ca.h"
#include "edge_clickhouse.h"
#include "edge_debug.h"
#include "edge_explain.h"
#include "edge_state_notify.h"
#include "edge_static.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "http1.h"
#include "protocol_events.h"

#define EDGE_HTTP_ACC_MAX (EDGE_STATE_VALUE_MAX + 4096)
/* Status + events ring (live sessions + progress log) need headroom. */
#define EDGE_HTTP_E7_JSON_MAX 32768

struct edge_http1_serve {
    http1_ctx_t         *h1;
    char                 method[16];
    char                 path[512];
    int                  headers_done;
    int                  done;
    edge_http1_docroot_t roots;
    edge_state_store_t  *store; /* not owned */
    edge_ws_hub_t       *hub;   /* not owned */
    edge_auth_ctx_t     *auth;  /* not owned */
    edge_principal_t     principal;
    edge_plugin_host_t  *plugins; /* not owned */
    edge_e7_callhome_t  *e7;      /* not owned; PR-5 */
    edge_clickhouse_t   *clickhouse; /* not owned */
    edge_ca_t           *ca;         /* not owned */
    int                  allow_blocking_dns;
    size_t               max_upstream_body;
    const char          *service_api_key; /* not owned */
    /** Optional CPE telemetry Basic Auth (not owned; from edge_config). */
    const char          *telemetry_user;
    const char          *telemetry_password;
    uint32_t             inbound_slot;    /* set by host; default 0 */

    /* Accumulated request for body (PUT/POST) — host buffer */
    uint8_t              acc[EDGE_HTTP_ACC_MAX];
    size_t               acc_len;
    long                 content_length; /* -1 unknown */
    size_t               body_off;       /* offset of body in acc after headers */
    int                  have_body_off;

    /* WebSocket upgrade (P1.7b) */
    int                  ws_upgrade; /* took /api/v1/stream upgrade */
    char                 ws_key[EDGE_WS_KEY_MAX];
    char                 request_id[EDGE_WS_REQUEST_ID];
};

static void note_response(edge_metrics_t *m, int status);
static void mark_body_off(edge_http1_serve_t *s);

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

static int build_response_ex(char *out, size_t out_cap, int status,
                             const char *reason, const char *ctype,
                             const char *extra_headers, const char *body,
                             size_t body_len, size_t *out_len)
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
    if (!extra_headers) {
        extra_headers = "";
    }
    n = snprintf(out, out_cap,
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "%s"
                 "Connection: close\r\n"
                 "\r\n",
                 status, reason, ctype, body_len, extra_headers);
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

static int build_response(char *out, size_t out_cap, int status,
                          const char *reason, const char *ctype,
                          const char *body, size_t body_len, size_t *out_len)
{
    return build_response_ex(out, out_cap, status, reason, ctype, NULL, body,
                             body_len, out_len);
}

static int auth_enforced(const edge_http1_serve_t *s)
{
    return s && s->auth && edge_auth_mode_enforced(s->auth->mode);
}

static int deny_unauthorized(edge_metrics_t *metrics, char *out, size_t out_cap,
                             size_t *out_len)
{
    static const char body[] = "{\"error\":\"UNAUTHORIZED\"}";
    if (build_response(out, out_cap, 401, "Unauthorized", "application/json",
                       body, sizeof(body) - 1, out_len) != 0) {
        return -1;
    }
    note_response(metrics, 401);
    return 1;
}

static int deny_forbidden(edge_metrics_t *metrics, char *out, size_t out_cap,
                          size_t *out_len)
{
    static const char body[] = "{\"error\":\"FORBIDDEN\"}";
    if (build_response(out, out_cap, 403, "Forbidden", "application/json", body,
                       sizeof(body) - 1, out_len) != 0) {
        return -1;
    }
    note_response(metrics, 403);
    return 1;
}

/** @return 0 allowed, 1 response written (deny), -1 hard error */
static int require_rbac(edge_http1_serve_t *s, edge_auth_resource_t res,
                        const char *ns, const char *key, edge_metrics_t *metrics,
                        char *out, size_t out_cap, size_t *out_len)
{
    edge_auth_decision_t d;

    if (!auth_enforced(s) || res == EDGE_RES_NONE) {
        return 0;
    }
    if (!s->principal.authenticated) {
        return deny_unauthorized(metrics, out, out_cap, out_len);
    }
    d = edge_auth_rbac_check(&s->principal, res, ns, key);
    if (d != EDGE_AUTH_ALLOW) {
        return deny_forbidden(metrics, out, out_cap, out_len);
    }
    return 0;
}

static int path_is_lab_login(const char *path)
{
    return path && (strcmp(path, "/auth/lab-login") == 0 ||
                    strncmp(path, "/auth/lab-login?", 16) == 0);
}

static int path_is_auth_me(const char *path)
{
    return path &&
           (strcmp(path, "/auth/me") == 0 || strncmp(path, "/auth/me?", 9) == 0);
}

static int wait_full_body(edge_http1_serve_t *s)
{
    size_t have = 0;
    if (s->content_length <= 0) {
        return 1;
    }
    if (!s->have_body_off) {
        mark_body_off(s);
    }
    if (s->have_body_off) {
        have = s->acc_len - s->body_off;
    }
    return have >= (size_t)s->content_length;
}

static int get_body(edge_http1_serve_t *s, const char **body, size_t *blen)
{
    if (!s->have_body_off) {
        mark_body_off(s);
    }
    if (!s->have_body_off) {
        *body = "";
        *blen = 0;
        return 0;
    }
    *body = (const char *)(s->acc + s->body_off);
    *blen = s->acc_len > s->body_off ? s->acc_len - s->body_off : 0;
    if (s->content_length >= 0 && *blen > (size_t)s->content_length) {
        *blen = (size_t)s->content_length;
    }
    return 1;
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

/* STATE_CHANGED fan-out goes through edge_state_*_and_notify (no double fire). */

static int dispatch_lab_login(edge_http1_serve_t *s, edge_metrics_t *metrics,
                              char *out, size_t out_cap, size_t *out_len)
{
    const char *body;
    size_t blen;
    char password[EDGE_AUTH_PASSWORD_MAX];
    char cookie_val[EDGE_AUTH_COOKIE_MAX];
    char set_cookie[EDGE_AUTH_COOKIE_MAX + 128];
    char json[256];
    edge_principal_t issued;
    int n;
    uint32_t ttl;

    if (!path_is_lab_login(s->path)) {
        return 0;
    }
    if (strcmp(s->method, "POST") != 0) {
        if (build_response(out, out_cap, 405, "Method Not Allowed",
                           "application/json", "{\"error\":\"METHOD\"}", 18,
                           out_len) != 0) {
            return -1;
        }
        note_response(metrics, 405);
        return 1;
    }
    if (!s->auth || s->auth->mode != EDGE_AUTH_MODE_LAB_PASSWORD) {
        if (build_response(out, out_cap, 503, "Service Unavailable",
                           "application/json",
                           "{\"error\":\"AUTH_DISABLED\"}", 26, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 503);
        return 1;
    }
    if (!wait_full_body(s)) {
        return 0;
    }
    get_body(s, &body, &blen);
    if (edge_auth_parse_login_password(body, blen, password, sizeof(password)) !=
        0) {
        if (build_response(out, out_cap, 400, "Bad Request", "application/json",
                           "{\"error\":\"BAD_BODY\"}", 19, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 400);
        return 1;
    }
    if (!edge_auth_password_ok(s->auth, password, strlen(password))) {
        if (build_response(out, out_cap, 401, "Unauthorized", "application/json",
                           "{\"error\":\"BAD_PASSWORD\"}", 24, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 401);
        return 1;
    }
    /*
     * Lab single-password login is the ops console for this process: grant
     * employee + employee_admin so E7 allowlist mutations (EDGE_RES_E7_ADMIN)
     * work from /e7/ without a separate proxy role ladder. Production uses
     * proxy_headers / OIDC to split employee vs employee_admin.
     */
    if (edge_auth_session_issue(
            s->auth, "lab",
            (uint32_t)(EDGE_ROLE_EMPLOYEE | EDGE_ROLE_EMPLOYEE_ADMIN),
            cookie_val, sizeof(cookie_val), &issued) != 0) {
        if (build_response(out, out_cap, 500, "Internal Server Error",
                           "application/json", "{\"error\":\"SESSION\"}", 19,
                           out_len) != 0) {
            return -1;
        }
        note_response(metrics, 500);
        return 1;
    }
    s->principal = issued;
    ttl = s->auth->session_ttl_s ? s->auth->session_ttl_s : 28800;
    n = snprintf(set_cookie, sizeof(set_cookie),
                 "Set-Cookie: %s=%s; HttpOnly; Path=/; SameSite=Lax; Max-Age=%u\r\n",
                 EDGE_AUTH_COOKIE_NAME, cookie_val, (unsigned)ttl);
    if (n < 0 || (size_t)n >= sizeof(set_cookie)) {
        return -1;
    }
    n = snprintf(json, sizeof(json),
                 "{\"ok\":true,\"sub\":\"%s\",\"roles\":[\"employee\","
                 "\"employee_admin\"],\"exp\":%lld}",
                 issued.sub, (long long)issued.exp);
    if (n < 0 || (size_t)n >= sizeof(json)) {
        return -1;
    }
    if (build_response_ex(out, out_cap, 200, "OK", "application/json", set_cookie,
                          json, (size_t)n, out_len) != 0) {
        return -1;
    }
    note_response(metrics, 200);
    return 1;
}

static int dispatch_auth_me(edge_http1_serve_t *s, edge_metrics_t *metrics,
                            char *out, size_t out_cap, size_t *out_len)
{
    char json[256];
    int n;

    if (!path_is_auth_me(s->path)) {
        return 0;
    }
    if (strcmp(s->method, "GET") != 0) {
        if (build_response(out, out_cap, 405, "Method Not Allowed",
                           "application/json", "{\"error\":\"METHOD\"}", 18,
                           out_len) != 0) {
            return -1;
        }
        note_response(metrics, 405);
        return 1;
    }
    if (!s->principal.authenticated) {
        return deny_unauthorized(metrics, out, out_cap, out_len);
    }
    n = snprintf(json, sizeof(json),
                 "{\"sub\":\"%s\",\"roles\":[\"employee\"],\"exp\":%lld}",
                 s->principal.sub, (long long)s->principal.exp);
    if (edge_auth_role_has(s->principal.roles, EDGE_ROLE_EMPLOYEE_ADMIN)) {
        n = snprintf(json, sizeof(json),
                     "{\"sub\":\"%s\",\"roles\":[\"employee\",\"employee_admin\"],"
                     "\"exp\":%lld}",
                     s->principal.sub, (long long)s->principal.exp);
    }
    if (n < 0 || (size_t)n >= sizeof(json)) {
        return -1;
    }
    if (build_response(out, out_cap, 200, "OK", "application/json", json,
                       (size_t)n, out_len) != 0) {
        return -1;
    }
    note_response(metrics, 200);
    return 1;
}

static int dispatch_stream(edge_http1_serve_t *s, edge_metrics_t *metrics,
                           char *out, size_t out_cap, size_t *out_len)
{
    int gate;

    if (!edge_ws_path_is_stream(s->path)) {
        return 0;
    }
    gate = require_rbac(s, EDGE_RES_WS_STREAM, NULL, NULL, metrics, out, out_cap,
                        out_len);
    if (gate != 0) {
        return gate;
    }
    if (strcmp(s->method, "GET") != 0) {
        if (build_response(out, out_cap, 405, "Method Not Allowed",
                           "application/json", "{\"error\":\"METHOD\"}", 18,
                           out_len) != 0) {
            return -1;
        }
        note_response(metrics, 405);
        return 1;
    }
    if (!http1_is_websocket_upgrade(s->h1) || !s->ws_key[0]) {
        if (build_response(out, out_cap, 400, "Bad Request", "application/json",
                           "{\"error\":\"WS_UPGRADE_REQUIRED\"}", 32,
                           out_len) != 0) {
            return -1;
        }
        note_response(metrics, 400);
        return 1;
    }
    if (edge_ws_build_101(s->ws_key, out, out_cap, out_len) != 0) {
        if (build_response(out, out_cap, 500, "Internal Server Error",
                           "application/json", "{\"error\":\"WS_ACCEPT\"}", 20,
                           out_len) != 0) {
            return -1;
        }
        note_response(metrics, 500);
        return 1;
    }
    s->ws_upgrade = 1;
    if (metrics) {
        metrics->requests++;
        metrics->responses_2xx++;
    }
    return 1;
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
    edge_auth_resource_t res;
    int gate;

    if (!path_is_state(s->path, ns, sizeof(ns), key, sizeof(key), prefix,
                       sizeof(prefix), &is_list)) {
        return 0; /* not state path */
    }
    res = edge_auth_classify(s->method, s->path, is_list);
    gate = require_rbac(s, res, ns, key[0] ? key : NULL, metrics, out, out_cap,
                        out_len);
    if (gate != 0) {
        return gate;
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
        er = edge_state_put_and_notify(s->store, s->hub, ns, key, body, blen,
                                       s->request_id, 0);
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
        er = edge_state_delete_and_notify(s->store, s->hub, ns, key,
                                          s->request_id, 0);
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

/**
 * Parse /api/v1/e7 path into components.
 * @return 1 if e7 path, 0 otherwise.
 * kinds: 0=status, 1=shelves list, 2=shelf, 3=disconnect, 4=commands,
 *        5=command get, 6=onts, 7=events (connection progress log),
 *        8=config capture POST, 9=config meta GET, 10=config onts GET,
 *        11=config full GET
 */
static int path_is_e7(const char *path, int *kind, char *mac_out, size_t mac_sz,
                      char *cmd_out, size_t cmd_sz, char *cursor_out,
                      size_t cursor_sz)
{
    const char *p;
    const char *q;
    char tmp[512];
    size_t plen;

    if (kind) {
        *kind = -1;
    }
    if (mac_out && mac_sz) {
        mac_out[0] = '\0';
    }
    if (cmd_out && cmd_sz) {
        cmd_out[0] = '\0';
    }
    if (cursor_out && cursor_sz) {
        cursor_out[0] = '\0';
    }
    if (!path || strncmp(path, "/api/v1/e7", 10) != 0) {
        return 0;
    }
    p = path + 10;
    if (*p == '?') {
        return 0;
    }
    if (*p == '\0') {
        return 0;
    }
    if (*p != '/') {
        return 0;
    }
    p++;
    q = strchr(p, '?');
    plen = q ? (size_t)(q - p) : strlen(p);
    if (plen >= sizeof(tmp)) {
        return 0;
    }
    memcpy(tmp, p, plen);
    tmp[plen] = '\0';
    p = tmp;

    if (strcmp(p, "status") == 0) {
        if (kind) {
            *kind = 0;
        }
        return 1;
    }
    if (strcmp(p, "events") == 0) {
        if (kind) {
            *kind = 7; /* connection progress log */
        }
        return 1;
    }
    if (strcmp(p, "shelves") == 0) {
        if (kind) {
            *kind = 1;
        }
        return 1;
    }
    if (strncmp(p, "shelves/", 8) == 0) {
        const char *rest = p + 8;
        const char *slash;
        size_t mlen;

        if (!rest[0]) {
            return 0;
        }
        slash = strchr(rest, '/');
        mlen = slash ? (size_t)(slash - rest) : strlen(rest);
        if (mlen == 0 || mlen >= mac_sz) {
            return 0;
        }
        memcpy(mac_out, rest, mlen);
        mac_out[mlen] = '\0';
        if (!slash) {
            if (kind) {
                *kind = 2; /* shelf detail / put / delete */
            }
            return 1;
        }
        rest = slash + 1;
        if (strcmp(rest, "disconnect") == 0) {
            if (kind) {
                *kind = 3;
            }
            return 1;
        }
        if (strcmp(rest, "commands") == 0) {
            if (kind) {
                *kind = 4;
            }
            return 1;
        }
        if (strncmp(rest, "commands/", 9) == 0) {
            const char *cid = rest + 9;
            size_t clen = strlen(cid);
            if (clen == 0 || clen >= cmd_sz) {
                return 0;
            }
            memcpy(cmd_out, cid, clen + 1);
            if (kind) {
                *kind = 5;
            }
            return 1;
        }
        if (strcmp(rest, "config/capture") == 0) {
            if (kind) {
                *kind = 8;
            }
            return 1;
        }
        if (strcmp(rest, "config") == 0) {
            if (kind) {
                *kind = 9;
            }
            return 1;
        }
        if (strcmp(rest, "config/onts") == 0 ||
            strncmp(rest, "config/onts?", 12) == 0) {
            if (kind) {
                *kind = 10;
            }
            return 1;
        }
        if (strcmp(rest, "config/full") == 0) {
            if (kind) {
                *kind = 11;
            }
            return 1;
        }
        if (strcmp(rest, "onts") == 0 || strncmp(rest, "onts?", 5) == 0) {
            if (kind) {
                *kind = 6;
            }
            return 1;
        }
        return 0;
    }
    return 0;
}

static void e7_parse_query(const char *path, char *cursor_out, size_t cursor_sz,
                           size_t *limit_out, uint64_t *since_id_out)
{
    const char *q;
    if (limit_out) {
        *limit_out = 0;
    }
    if (since_id_out) {
        *since_id_out = 0;
    }
    if (cursor_out && cursor_sz) {
        cursor_out[0] = '\0';
    }
    if (!path) {
        return;
    }
    q = strchr(path, '?');
    if (!q) {
        return;
    }
    q++;
    while (*q) {
        if (strncmp(q, "cursor=", 7) == 0) {
            size_t i = 0;
            q += 7;
            while (q[i] && q[i] != '&' && i + 1 < cursor_sz) {
                cursor_out[i] = q[i];
                i++;
            }
            cursor_out[i] = '\0';
            q += i;
        } else if (strncmp(q, "limit=", 6) == 0) {
            q += 6;
            if (limit_out) {
                *limit_out = (size_t)strtoul(q, NULL, 10);
            }
            while (*q && *q != '&') {
                q++;
            }
        } else if (strncmp(q, "since=", 6) == 0) {
            q += 6;
            if (since_id_out) {
                *since_id_out = (uint64_t)strtoull(q, NULL, 10);
            }
            while (*q && *q != '&') {
                q++;
            }
        } else {
            while (*q && *q != '&') {
                q++;
            }
        }
        if (*q == '&') {
            q++;
        }
    }
}

/** Extract "rpc_xml" or "op" string from small JSON body. */
static int e7_parse_command_body(const char *body, size_t blen, char *rpc_out,
                                 size_t rpc_sz, char *op_out, size_t op_sz)
{
    char tmp[EDGE_HTTP_ACC_MAX];
    size_t n;
    const char *p;
    const char *key;

    if (rpc_out && rpc_sz) {
        rpc_out[0] = '\0';
    }
    if (op_out && op_sz) {
        op_out[0] = '\0';
    }
    if (!body || blen == 0) {
        return -1;
    }
    n = blen < sizeof(tmp) - 1 ? blen : sizeof(tmp) - 1;
    memcpy(tmp, body, n);
    tmp[n] = '\0';

    key = strstr(tmp, "\"rpc_xml\"");
    if (key) {
        p = strchr(key + 9, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == '"') {
                size_t i = 0;
                p++;
                while (*p && *p != '"' && i + 1 < rpc_sz) {
                    if (*p == '\\' && p[1]) {
                        p++;
                    }
                    rpc_out[i++] = *p++;
                }
                rpc_out[i] = '\0';
                return 0;
            }
        }
    }
    key = strstr(tmp, "\"op\"");
    if (key) {
        p = strchr(key + 4, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == '"') {
                size_t i = 0;
                p++;
                while (*p && *p != '"' && i + 1 < op_sz) {
                    op_out[i++] = *p++;
                }
                op_out[i] = '\0';
                return 0;
            }
        }
    }
    return -1;
}

/** Extract string field "key":"value" from small JSON body (best-effort). */
static void e7_json_str_field(const char *tmp, const char *key, char *out,
                              size_t out_sz)
{
    char needle[64];
    const char *p;
    size_t i = 0;
    if (!tmp || !key || !out || out_sz == 0) {
        return;
    }
    out[0] = '\0';
    if (snprintf(needle, sizeof(needle), "\"%s\"", key) >= (int)sizeof(needle)) {
        return;
    }
    p = strstr(tmp, needle);
    if (!p) {
        return;
    }
    p = strchr(p + strlen(needle), ':');
    if (!p) {
        return;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return;
    }
    p++;
    while (*p && *p != '"' && i + 1 < out_sz) {
        out[i++] = *p++;
    }
    out[i] = '\0';
}

/** Extract optional shelf PUT fields from JSON body. */
static void e7_parse_shelf_body(const char *body, size_t blen, char *label_out,
                                size_t label_sz, int *enabled_out,
                                char *vendor_out, size_t vendor_sz,
                                char *device_id_out, size_t device_id_sz,
                                char *secret_out, size_t secret_sz,
                                int *secret_present)
{
    char tmp[2048];
    size_t n;

    if (label_out && label_sz) {
        label_out[0] = '\0';
    }
    if (vendor_out && vendor_sz) {
        vendor_out[0] = '\0';
    }
    if (device_id_out && device_id_sz) {
        device_id_out[0] = '\0';
    }
    if (secret_out && secret_sz) {
        secret_out[0] = '\0';
    }
    if (secret_present) {
        *secret_present = 0;
    }
    if (enabled_out) {
        *enabled_out = 1;
    }
    if (!body || blen == 0) {
        return;
    }
    n = blen < sizeof(tmp) - 1 ? blen : sizeof(tmp) - 1;
    memcpy(tmp, body, n);
    tmp[n] = '\0';
    e7_json_str_field(tmp, "label", label_out, label_sz);
    e7_json_str_field(tmp, "vendor", vendor_out, vendor_sz);
    e7_json_str_field(tmp, "device_id", device_id_out, device_id_sz);
    if (strstr(tmp, "\"secret\"")) {
        if (secret_present) {
            *secret_present = 1;
        }
        e7_json_str_field(tmp, "secret", secret_out, secret_sz);
    }
    if (strstr(tmp, "\"enabled\":false") || strstr(tmp, "\"enabled\": false") ||
        strstr(tmp, "\"enabled\":0") || strstr(tmp, "\"enabled\": 0")) {
        if (enabled_out) {
            *enabled_out = 0;
        }
    }
}

static int explain_templates_root(const edge_http1_serve_t *s, char *path,
                                  size_t path_sz)
{
    if (!s || !s->roots.spa_root || !s->roots.spa_root[0] || !path ||
        path_sz < 16) {
        return -1;
    }
    if (snprintf(path, path_sz, "%s/explain/templates", s->roots.spa_root) < 0) {
        return -1;
    }
    return 0;
}

static int path_is_explain_templates(const char *path)
{
    return path &&
           (strcmp(path, "/api/v1/explain/templates") == 0 ||
            strncmp(path, "/api/v1/explain/templates?", 26) == 0);
}

static int path_is_explain_render(const char *path)
{
    return path &&
           (strcmp(path, "/api/v1/explain/render") == 0 ||
            strncmp(path, "/api/v1/explain/render?", 23) == 0);
}

static int dispatch_explain(edge_http1_serve_t *s, edge_metrics_t *metrics,
                            char *out, size_t out_cap, size_t *out_len)
{
    int gate;
    char root[512];
    char jbuf[EDGE_HTTP_E7_JSON_MAX];
    int jn;

    if (strncmp(s->path, "/api/v1/explain", 15) != 0) {
        return 0;
    }

    gate = require_rbac(s, EDGE_RES_EXPLAIN, NULL, NULL, metrics, out, out_cap,
                        out_len);
    if (gate != 0) {
        return gate;
    }

    if (explain_templates_root(s, root, sizeof(root)) != 0) {
        static const char body[] = "{\"error\":\"SPA_ROOT_UNSET\"}";
        if (build_response(out, out_cap, 503, "Service Unavailable",
                           "application/json", body, sizeof(body) - 1,
                           out_len) != 0) {
            return -1;
        }
        note_response(metrics, 503);
        return 1;
    }

    if (path_is_explain_templates(s->path) && strcmp(s->method, "GET") == 0) {
        jn = edge_explain_list_templates_json(root, jbuf, sizeof(jbuf));
        if (jn < 0) {
            return -1;
        }
        if (build_response(out, out_cap, 200, "OK", "application/json", jbuf,
                           (size_t)jn, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 200);
        return 1;
    }

    if (path_is_explain_render(s->path) && strcmp(s->method, "POST") == 0) {
        const char *body = NULL;
        size_t blen = 0;
        char *big = NULL;
        const size_t big_cap = 131072;
        get_body(s, &body, &blen);
        big = (char *)malloc(big_cap);
        if (!big) {
            return -1;
        }
        jn = edge_explain_render_json(root, body ? body : "", blen, big, big_cap);
        if (jn < 0) {
            free(big);
            return -1;
        }
        if (build_response(out, out_cap, 200, "OK", "application/json", big,
                           (size_t)jn, out_len) != 0) {
            free(big);
            return -1;
        }
        note_response(metrics, 200);
        free(big);
        return 1;
    }

    {
        static const char body[] = "{\"error\":\"not_found\"}";
        if (build_response(out, out_cap, 404, "Not Found", "application/json",
                           body, sizeof(body) - 1, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 404);
        return 1;
    }
}

/**
 * /api/v1/ca/ routes and GET /ca/crl.pem — Certificate Authority.
 * Admin routes require employee_admin (E7_ADMIN); CSR sign allows employee.
 * Public CRL is open (no session).
 */
static int dispatch_ca(edge_http1_serve_t *s, edge_metrics_t *metrics, char *out,
                       size_t out_cap, size_t *out_len)
{
    int gate;
    int status = 500;
    int rc;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    int is_public_crl =
        (strcmp(s->path, "/ca/crl.pem") == 0 ||
         strcmp(s->path, "/api/v1/ca/crl.pem") == 0) &&
        strcmp(s->method, "GET") == 0;
    int is_sign = (strstr(s->path, "/api/v1/ca/sign") == s->path) &&
                  strcmp(s->method, "POST") == 0;
    int is_admin = !is_public_crl && !is_sign &&
                   (strcmp(s->method, "POST") == 0 ||
                    strstr(s->path, "/revoke") != NULL ||
                    strstr(s->path, "/authorities") != NULL);

    if (!is_public_crl) {
        gate = require_rbac(s, is_admin ? EDGE_RES_E7_ADMIN : EDGE_RES_E7_GET,
                            NULL, NULL, metrics, out, out_cap, out_len);
        if (gate != 0) {
            return gate;
        }
    }

    if (strcmp(s->method, "POST") == 0) {
        if (!wait_full_body(s) && s->content_length > 0) {
            return 0;
        }
        if (s->have_body_off && s->acc_len > s->body_off) {
            body = s->acc + s->body_off;
            body_len = s->acc_len - s->body_off;
            if (s->content_length >= 0 && body_len > (size_t)s->content_length) {
                body_len = (size_t)s->content_length;
            }
        }
    }

    rc = edge_ca_http_dispatch(s->ca, s->method, s->path, body, body_len, out,
                               out_cap, out_len, &status);
    if (rc == 0) {
        return 0;
    }
    if (rc < 0) {
        return -1;
    }
    note_response(metrics, status);
    return 1;
}

/**
 * POST /api/v1/telemetry/events — CPE / agent JSON into ClickHouse batch.
 * Body: one JSON object, or NDJSON (multiple objects separated by newlines),
 * or {"events":[ {...}, ... ]}.
 * GET /api/v1/telemetry/status — aggregator stats.
 */
static int dispatch_telemetry(edge_http1_serve_t *s, edge_metrics_t *metrics,
                              char *out, size_t out_cap, size_t *out_len)
{
    int gate;
    char jbuf[512];
    int jn;
    const char *p;
    const uint8_t *body;
    size_t body_len;

    if (strncmp(s->path, "/api/v1/telemetry", 17) != 0) {
        return 0;
    }
    p = s->path + 17;
    if (*p == '?') {
        return 0;
    }
    if (*p != '\0' && *p != '/') {
        return 0;
    }
    if (*p == '/') {
        p++;
    }

    /* Prefer telemetry Basic Auth → ingest role, else employee session. */
    if (!s->principal.authenticated && s->telemetry_user &&
        s->telemetry_user[0]) {
        char authz[384];
        if (http1_get_header(s->h1, "Authorization", authz, sizeof(authz)) ==
                0 &&
            strncmp(authz, "Basic ", 6) == 0) {
            const char *b64 = authz + 6;
            unsigned char raw[192];
            size_t raw_len = 0;
            size_t bi = 0;
            size_t blen = strlen(b64);
            int ok_dec = 1;

            while (blen > 0 &&
                   (b64[blen - 1] == '\r' || b64[blen - 1] == '\n' ||
                    b64[blen - 1] == ' ' || b64[blen - 1] == '\t')) {
                blen--;
            }
            while (bi + 3 < blen && raw_len + 3 < sizeof(raw) && ok_dec) {
                int vals[4];
                size_t k;
                for (k = 0; k < 4; k++) {
                    unsigned char c = (unsigned char)b64[bi + k];
                    if (c >= 'A' && c <= 'Z') {
                        vals[k] = c - 'A';
                    } else if (c >= 'a' && c <= 'z') {
                        vals[k] = c - 'a' + 26;
                    } else if (c >= '0' && c <= '9') {
                        vals[k] = c - '0' + 52;
                    } else if (c == '+') {
                        vals[k] = 62;
                    } else if (c == '/') {
                        vals[k] = 63;
                    } else if (c == '=') {
                        vals[k] = -2; /* pad */
                    } else {
                        ok_dec = 0;
                        break;
                    }
                }
                if (!ok_dec) {
                    break;
                }
                if (vals[0] < 0 || vals[1] < 0) {
                    ok_dec = 0;
                    break;
                }
                raw[raw_len++] =
                    (unsigned char)((vals[0] << 2) | (vals[1] >> 4));
                if (vals[2] >= 0) {
                    raw[raw_len++] =
                        (unsigned char)(((vals[1] & 15) << 4) | (vals[2] >> 2));
                }
                if (vals[3] >= 0) {
                    raw[raw_len++] =
                        (unsigned char)(((vals[2] & 3) << 6) | vals[3]);
                }
                bi += 4;
            }
            if (ok_dec && raw_len > 0) {
                char userpass[192];
                char *colon;
                if (raw_len >= sizeof(userpass)) {
                    raw_len = sizeof(userpass) - 1;
                }
                memcpy(userpass, raw, raw_len);
                userpass[raw_len] = '\0';
                colon = strchr(userpass, ':');
                if (colon) {
                    *colon = '\0';
                    if (strcmp(userpass, s->telemetry_user) == 0 &&
                        strcmp(colon + 1,
                               s->telemetry_password
                                   ? s->telemetry_password
                                   : "") == 0) {
                        s->principal.authenticated = 1;
                        snprintf(s->principal.sub, sizeof(s->principal.sub),
                                 "cpe_ingest");
                        s->principal.roles = EDGE_ROLE_INGEST;
                    }
                }
            }
        }
    }

    gate = require_rbac(s, EDGE_RES_TELEMETRY, NULL, NULL, metrics, out,
                        out_cap, out_len);
    if (gate != 0) {
        return gate;
    }

    if ((p[0] == '\0' || strcmp(p, "status") == 0) &&
        strcmp(s->method, "GET") == 0) {
        jn = edge_clickhouse_status_json(s->clickhouse, jbuf, sizeof(jbuf));
        if (jn < 0) {
            static const char bodyz[] = "{\"enabled\":false}";
            if (build_response(out, out_cap, 200, "OK", "application/json",
                               bodyz, sizeof(bodyz) - 1, out_len) != 0) {
                return -1;
            }
            note_response(metrics, 200);
            return 1;
        }
        if (build_response(out, out_cap, 200, "OK", "application/json", jbuf,
                           (size_t)jn, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 200);
        return 1;
    }

    if ((strcmp(p, "events") == 0 || strncmp(p, "events?", 7) == 0) &&
        strcmp(s->method, "POST") == 0) {
        size_t queued = 0;
        size_t i = 0;
        int rc = 0;

        if (!edge_clickhouse_enabled(s->clickhouse)) {
            static const char bodyz[] =
                "{\"ok\":false,\"error\":\"CLICKHOUSE_DISABLED\"}";
            if (build_response(out, out_cap, 503, "Service Unavailable",
                               "application/json", bodyz, sizeof(bodyz) - 1,
                               out_len) != 0) {
                return -1;
            }
            note_response(metrics, 503);
            return 1;
        }
        if (!wait_full_body(s) && s->content_length > 0) {
            return 0; /* need more body bytes */
        }
        if (!s->have_body_off) {
            return 0;
        }
        body = s->acc + s->body_off;
        body_len = s->acc_len > s->body_off ? s->acc_len - s->body_off : 0;
        if (s->content_length >= 0 && body_len > (size_t)s->content_length) {
            body_len = (size_t)s->content_length;
        }
        if (body_len == 0) {
            static const char bodyz[] = "{\"ok\":false,\"error\":\"EMPTY_BODY\"}";
            if (build_response(out, out_cap, 400, "Bad Request",
                               "application/json", bodyz, sizeof(bodyz) - 1,
                               out_len) != 0) {
                return -1;
            }
            note_response(metrics, 400);
            return 1;
        }

        /* NDJSON or single object */
        while (i < body_len) {
            size_t start;
            size_t end;
            while (i < body_len &&
                   (body[i] == ' ' || body[i] == '\n' || body[i] == '\r' ||
                    body[i] == '\t')) {
                i++;
            }
            if (i >= body_len) {
                break;
            }
            if (body[i] != '{') {
                /* skip non-object (e.g. array wrapper start) */
                if (body[i] == '[') {
                    i++;
                    continue;
                }
                break;
            }
            start = i;
            {
                int depth = 0;
                int in_str = 0;
                int esc = 0;
                for (; i < body_len; i++) {
                    char c = (char)body[i];
                    if (in_str) {
                        if (esc) {
                            esc = 0;
                        } else if (c == '\\') {
                            esc = 1;
                        } else if (c == '"') {
                            in_str = 0;
                        }
                        continue;
                    }
                    if (c == '"') {
                        in_str = 1;
                    } else if (c == '{') {
                        depth++;
                    } else if (c == '}') {
                        depth--;
                        if (depth == 0) {
                            i++;
                            break;
                        }
                    }
                }
                end = i;
            }
            if (end > start) {
                rc = edge_clickhouse_enqueue_cpe_ndjson(
                    s->clickhouse, (const char *)body + start, end - start);
                if (rc == 0) {
                    queued++;
                }
            }
            while (i < body_len && (body[i] == ',' || body[i] == ' ' ||
                                    body[i] == '\n' || body[i] == '\r')) {
                i++;
            }
        }

        {
            ch_async_stats_t st;
            edge_clickhouse_stats(s->clickhouse, &st);
            jn = snprintf(jbuf, sizeof(jbuf),
                          "{\"ok\":%s,\"queued\":%zu,\"rows_queued\":%llu,"
                          "\"flush_err\":%llu}",
                          queued > 0 ? "true" : "false", queued,
                          (unsigned long long)st.rows_queued,
                          (unsigned long long)st.flush_err);
        }
        if (jn < 0) {
            return -1;
        }
        if (build_response(out, out_cap, queued > 0 ? 202 : 400, "Accepted",
                           "application/json", jbuf, (size_t)jn, out_len) !=
            0) {
            return -1;
        }
        note_response(metrics, queued > 0 ? 202 : 400);
        return 1;
    }

    {
        static const char bodyz[] = "{\"error\":\"NOT_FOUND\"}";
        if (build_response(out, out_cap, 404, "Not Found", "application/json",
                           bodyz, sizeof(bodyz) - 1, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 404);
        return 1;
    }
}

/**
 * /api/v1/debug/memory
 * /api/v1/debug/cpu/capabilities
 * /api/v1/debug/cpu/profile  GET status|flame|folded, POST start
 */
static int dispatch_debug(edge_http1_serve_t *s, edge_metrics_t *metrics,
                          char *out, size_t out_cap, size_t *out_len)
{
    static char jbuf[98304];
    int jn;
    int gate;
    const char *p;
    const char *ctype = "application/json";

    if (strncmp(s->path, "/api/v1/debug", 13) != 0) {
        return 0;
    }
    p = s->path + 13;
    if (*p == '?') {
        return 0;
    }
    if (*p != '\0' && *p != '/') {
        return 0;
    }

    gate = require_rbac(s, EDGE_RES_E7_GET, NULL, NULL, metrics, out, out_cap,
                        out_len);
    if (gate != 0) {
        return gate;
    }

    /* strip query */
    {
        static char path[256];
        size_t i = 0;
        const char *src = s->path;
        while (*src && *src != '?' && i + 1 < sizeof(path)) {
            path[i++] = *src++;
        }
        path[i] = '\0';
        p = path + 13;
        if (*p == '/') {
            p++;
        }

        if (strcmp(p, "memory") == 0 && strcmp(s->method, "GET") == 0) {
            jn = edge_debug_memory_json(metrics, s->store, s->e7, jbuf,
                                        sizeof(jbuf));
            if (jn < 0) {
                static const char body[] = "{\"error\":\"MEMORY_OVERFLOW\"}";
                if (build_response(out, out_cap, 500, "Internal Server Error",
                                   "application/json", body, sizeof(body) - 1,
                                   out_len) != 0) {
                    return -1;
                }
                note_response(metrics, 500);
                return 1;
            }
            if (build_response(out, out_cap, 200, "OK", ctype, jbuf, (size_t)jn,
                               out_len) != 0) {
                return -1;
            }
            note_response(metrics, 200);
            return 1;
        }

        if (strcmp(p, "cpu/capabilities") == 0 &&
            strcmp(s->method, "GET") == 0) {
            jn = edge_debug_cpu_capabilities_json(jbuf, sizeof(jbuf));
            if (jn < 0) {
                return -1;
            }
            if (build_response(out, out_cap, 200, "OK", ctype, jbuf, (size_t)jn,
                               out_len) != 0) {
                return -1;
            }
            note_response(metrics, 200);
            return 1;
        }

        if (strncmp(p, "cpu/profile", 11) == 0 &&
            (p[11] == '\0' || p[11] == '/')) {
            const char *sub = p + 11;
            if (*sub == '/') {
                sub++;
            }

            if (strcmp(s->method, "POST") == 0 &&
                (sub[0] == '\0' || strcmp(sub, "start") == 0)) {
                char err[128];
                int seconds = 10;
                const char *mode = "auto";
                /* optional ?seconds=N&mode=sigprof from path query */
                {
                    const char *q = strchr(s->path, '?');
                    if (q) {
                        const char *sp = strstr(q, "seconds=");
                        const char *mp = strstr(q, "mode=");
                        if (sp) {
                            seconds = atoi(sp + 8);
                        }
                        if (mp) {
                            static char mbuf[16];
                            size_t mi = 0;
                            mp += 5;
                            while (*mp && *mp != '&' && mi + 1 < sizeof(mbuf)) {
                                mbuf[mi++] = *mp++;
                            }
                            mbuf[mi] = '\0';
                            mode = mbuf;
                        }
                    }
                }
                if (edge_debug_cpu_profile_start(seconds, mode, err,
                                                 sizeof(err)) != 0) {
                    jn = snprintf(jbuf, sizeof(jbuf),
                                  "{\"ok\":false,\"error\":\"%s\"}",
                                  err[0] ? err : "start_failed");
                    if (jn < 0) {
                        return -1;
                    }
                    if (build_response(out, out_cap, 409, "Conflict", ctype,
                                       jbuf, (size_t)jn, out_len) != 0) {
                        return -1;
                    }
                    note_response(metrics, 409);
                    return 1;
                }
                jn = edge_debug_cpu_profile_status_json(jbuf, sizeof(jbuf));
                if (jn < 0) {
                    return -1;
                }
                if (build_response(out, out_cap, 200, "OK", ctype, jbuf,
                                   (size_t)jn, out_len) != 0) {
                    return -1;
                }
                note_response(metrics, 200);
                return 1;
            }

            if (strcmp(s->method, "GET") == 0) {
                if (strcmp(sub, "flame") == 0) {
                    jn = edge_debug_cpu_profile_flame_json(jbuf, sizeof(jbuf));
                } else if (strcmp(sub, "folded") == 0) {
                    jn = edge_debug_cpu_profile_folded(jbuf, sizeof(jbuf));
                    ctype = "text/plain";
                } else {
                    jn = edge_debug_cpu_profile_status_json(jbuf, sizeof(jbuf));
                }
                if (jn < 0) {
                    return -1;
                }
                if (build_response(out, out_cap, 200, "OK", ctype, jbuf,
                                   (size_t)jn, out_len) != 0) {
                    return -1;
                }
                note_response(metrics, 200);
                return 1;
            }

            if (build_response(out, out_cap, 405, "Method Not Allowed",
                               "text/plain", "method not allowed\n", 19,
                               out_len) != 0) {
                return -1;
            }
            note_response(metrics, 405);
            return 1;
        }
    }

    {
        static const char body[] = "{\"error\":\"NOT_FOUND\"}";
        if (build_response(out, out_cap, 404, "Not Found", "application/json",
                           body, sizeof(body) - 1, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 404);
        return 1;
    }
}

static int dispatch_e7(edge_http1_serve_t *s, edge_metrics_t *metrics, char *out,
                       size_t out_cap, size_t *out_len)
{
    int kind = -1;
    char mac[64];
    char cmd_id[EDGE_E7_CMD_ID_MAX];
    char cursor[EDGE_STATE_KEY_MAX];
    size_t limit = 0;
    uint64_t since_id = 0;
    edge_auth_resource_t res;
    int gate;
    char jbuf[EDGE_HTTP_E7_JSON_MAX];
    int jn;
    int status;
    const char *reason;

    if (!path_is_e7(s->path, &kind, mac, sizeof(mac), cmd_id, sizeof(cmd_id),
                    cursor, sizeof(cursor))) {
        return 0;
    }
    e7_parse_query(s->path, cursor, sizeof(cursor), &limit, &since_id);

    res = edge_auth_classify(s->method, s->path, 0);
    gate = require_rbac(s, res, NULL, NULL, metrics, out, out_cap, out_len);
    if (gate != 0) {
        return gate;
    }

    if (!s->e7) {
        static const char body[] = "{\"error\":\"E7_UNAVAILABLE\"}";
        if (build_response(out, out_cap, 503, "Service Unavailable",
                           "application/json", body, sizeof(body) - 1,
                           out_len) != 0) {
            return -1;
        }
        note_response(metrics, 503);
        return 1;
    }

    /* GET status */
    if (kind == 0 && strcmp(s->method, "GET") == 0) {
        jn = edge_e7_callhome_status_json(s->e7, jbuf, sizeof(jbuf));
        if (jn < 0) {
            return -1;
        }
        if (build_response(out, out_cap, 200, "OK", "application/json", jbuf,
                           (size_t)jn, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 200);
        return 1;
    }

    /* GET shelves list */
    if (kind == 1 && strcmp(s->method, "GET") == 0) {
        jn = edge_e7_callhome_shelves_json(s->e7, jbuf, sizeof(jbuf));
        if (jn < 0) {
            return -1;
        }
        if (build_response(out, out_cap, 200, "OK", "application/json", jbuf,
                           (size_t)jn, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 200);
        return 1;
    }

    /* shelf detail GET / PUT / DELETE */
    if (kind == 2) {
        if (strcmp(s->method, "GET") == 0) {
            jn = edge_e7_callhome_shelf_json(s->e7, mac, jbuf, sizeof(jbuf));
            if (jn == -2) {
                static const char body[] = "{\"error\":\"NOT_FOUND\"}";
                if (build_response(out, out_cap, 404, "Not Found",
                                   "application/json", body, sizeof(body) - 1,
                                   out_len) != 0) {
                    return -1;
                }
                note_response(metrics, 404);
                return 1;
            }
            if (jn < 0) {
                static const char body[] = "{\"error\":\"BAD_MAC\"}";
                if (build_response(out, out_cap, 400, "Bad Request",
                                   "application/json", body, sizeof(body) - 1,
                                   out_len) != 0) {
                    return -1;
                }
                note_response(metrics, 400);
                return 1;
            }
            if (build_response(out, out_cap, 200, "OK", "application/json", jbuf,
                               (size_t)jn, out_len) != 0) {
                return -1;
            }
            note_response(metrics, 200);
            return 1;
        }
        if (strcmp(s->method, "PUT") == 0) {
            const char *body;
            size_t blen;
            char label[EDGE_CONFIG_E7_SHELF_ID_MAX];
            int enabled = 1;
            if (!wait_full_body(s) && s->content_length > 0) {
                return 0;
            }
            {
                char vendor[16];
                char device_id[128];
                char secret[128];
                int secret_present = 0;
                get_body(s, &body, &blen);
                e7_parse_shelf_body(body, blen, label, sizeof(label), &enabled,
                                    vendor, sizeof(vendor), device_id,
                                    sizeof(device_id), secret, sizeof(secret),
                                    &secret_present);
                if (edge_e7_callhome_allowlist_upsert_ex(
                        s->e7, mac, label[0] ? label : NULL, enabled,
                        vendor[0] ? vendor : NULL,
                        device_id[0] ? device_id : NULL,
                        secret_present ? secret : NULL) != 0) {
                    static const char eb[] = "{\"error\":\"UPSERT_FAILED\"}";
                    if (build_response(out, out_cap, 400, "Bad Request",
                                       "application/json", eb, sizeof(eb) - 1,
                                       out_len) != 0) {
                        return -1;
                    }
                    note_response(metrics, 400);
                    return 1;
                }
            }
            jn = edge_e7_callhome_shelf_json(s->e7, mac, jbuf, sizeof(jbuf));
            if (jn < 0) {
                jn = snprintf(jbuf, sizeof(jbuf),
                              "{\"v\":1,\"mac\":\"%s\",\"ok\":true,"
                              "\"note\":\"runtime allowlist is non-durable\"}",
                              mac);
            }
            if (build_response(out, out_cap, 200, "OK", "application/json", jbuf,
                               (size_t)jn, out_len) != 0) {
                return -1;
            }
            note_response(metrics, 200);
            return 1;
        }
        if (strcmp(s->method, "DELETE") == 0) {
            if (edge_e7_callhome_allowlist_delete(s->e7, mac) != 0) {
                static const char body[] = "{\"error\":\"NOT_FOUND\"}";
                if (build_response(out, out_cap, 404, "Not Found",
                                   "application/json", body, sizeof(body) - 1,
                                   out_len) != 0) {
                    return -1;
                }
                note_response(metrics, 404);
                return 1;
            }
            if (build_response(out, out_cap, 204, "No Content",
                               "application/json", "", 0, out_len) != 0) {
                return -1;
            }
            note_response(metrics, 204);
            return 1;
        }
    }

    /* POST disconnect */
    if (kind == 3 && strcmp(s->method, "POST") == 0) {
        if (edge_e7_callhome_disconnect(s->e7, mac) != 0) {
            static const char body[] = "{\"error\":\"BAD_MAC\"}";
            if (build_response(out, out_cap, 400, "Bad Request",
                               "application/json", body, sizeof(body) - 1,
                               out_len) != 0) {
                return -1;
            }
            note_response(metrics, 400);
            return 1;
        }
        jn = snprintf(jbuf, sizeof(jbuf),
                      "{\"v\":1,\"mac\":\"%s\",\"disconnected\":true}", mac);
        if (build_response(out, out_cap, 200, "OK", "application/json", jbuf,
                           (size_t)jn, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 200);
        return 1;
    }

    /* POST commands */
    if (kind == 4 && strcmp(s->method, "POST") == 0) {
        const char *body;
        size_t blen;
        char rpc_xml[EDGE_HTTP_ACC_MAX];
        char op[64];
        char out_cmd[EDGE_E7_CMD_ID_MAX];
        int http_st = 400;

        if (!wait_full_body(s) && s->content_length > 0) {
            return 0;
        }
        get_body(s, &body, &blen);
        rpc_xml[0] = '\0';
        op[0] = '\0';
        (void)e7_parse_command_body(body, blen, rpc_xml, sizeof(rpc_xml), op,
                                    sizeof(op));
        if (edge_e7_callhome_command_submit(
                s->e7, mac, rpc_xml[0] ? rpc_xml : NULL,
                rpc_xml[0] ? strlen(rpc_xml) : 0, op[0] ? op : NULL, out_cmd,
                sizeof(out_cmd), &http_st) != 0) {
            const char *eb;
            if (http_st == 409) {
                eb = "{\"error\":\"NO_SESSION\"}";
                reason = "Conflict";
            } else if (http_st == 503) {
                eb = "{\"error\":\"SESSION_NOT_OPEN\"}";
                reason = "Service Unavailable";
            } else if (http_st == 429) {
                eb = "{\"error\":\"TOO_MANY_COMMANDS\"}";
                reason = "Too Many Requests";
            } else {
                eb = "{\"error\":\"BAD_COMMAND\"}";
                reason = "Bad Request";
                http_st = 400;
            }
            if (build_response(out, out_cap, http_st, reason, "application/json",
                               eb, strlen(eb), out_len) != 0) {
                return -1;
            }
            note_response(metrics, http_st);
            return 1;
        }
        jn = snprintf(jbuf, sizeof(jbuf),
                      "{\"v\":1,\"cmd_id\":\"%s\",\"mac\":\"%s\","
                      "\"status\":\"pending\"}",
                      out_cmd, mac);
        if (build_response(out, out_cap, 202, "Accepted", "application/json",
                           jbuf, (size_t)jn, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 202);
        return 1;
    }

    /* GET command result */
    if (kind == 5 && strcmp(s->method, "GET") == 0) {
        jn = edge_e7_callhome_command_json(s->e7, mac, cmd_id, jbuf,
                                           sizeof(jbuf));
        if (jn < 0) {
            static const char body[] = "{\"error\":\"NOT_FOUND\"}";
            if (build_response(out, out_cap, 404, "Not Found",
                               "application/json", body, sizeof(body) - 1,
                               out_len) != 0) {
                return -1;
            }
            note_response(metrics, 404);
            return 1;
        }
        if (build_response(out, out_cap, 200, "OK", "application/json", jbuf,
                           (size_t)jn, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 200);
        return 1;
    }

    /* GET onts */
    if (kind == 6 && strcmp(s->method, "GET") == 0) {
        jn = edge_e7_callhome_onts_json(s->e7, mac, cursor[0] ? cursor : NULL,
                                        limit, jbuf, sizeof(jbuf));
        if (jn < 0) {
            static const char body[] = "{\"error\":\"BAD_MAC\"}";
            if (build_response(out, out_cap, 400, "Bad Request",
                               "application/json", body, sizeof(body) - 1,
                               out_len) != 0) {
                return -1;
            }
            note_response(metrics, 400);
            return 1;
        }
        if (build_response(out, out_cap, 200, "OK", "application/json", jbuf,
                           (size_t)jn, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 200);
        return 1;
    }

    /* POST config/capture — get-config + inventory pipeline */
    if (kind == 8 && strcmp(s->method, "POST") == 0) {
        char out_cmd[EDGE_E7_CMD_ID_MAX];
        int http_st = 400;
        if (edge_e7_callhome_config_capture(s->e7, mac, out_cmd, sizeof(out_cmd),
                                            &http_st) != 0) {
            const char *eb;
            const char *reason = "Bad Request";
            if (http_st == 409) {
                eb = "{\"error\":\"NO_SESSION\"}";
                reason = "Conflict";
            } else if (http_st == 503) {
                eb = "{\"error\":\"SESSION_NOT_OPEN\"}";
                reason = "Service Unavailable";
            } else if (http_st == 429) {
                eb = "{\"error\":\"TOO_MANY_COMMANDS\"}";
                reason = "Too Many Requests";
            } else {
                eb = "{\"error\":\"BAD_COMMAND\"}";
                http_st = 400;
            }
            if (build_response(out, out_cap, http_st, reason, "application/json",
                               eb, strlen(eb), out_len) != 0) {
                return -1;
            }
            note_response(metrics, http_st);
            return 1;
        }
        jn = snprintf(jbuf, sizeof(jbuf),
                      "{\"v\":1,\"cmd_id\":\"%s\",\"mac\":\"%s\","
                      "\"status\":\"pending\",\"op\":\"get-config\"}",
                      out_cmd, mac);
        if (build_response(out, out_cap, 202, "Accepted", "application/json",
                           jbuf, (size_t)jn, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 202);
        return 1;
    }

    /* GET config meta */
    if (kind == 9 && strcmp(s->method, "GET") == 0) {
        jn = edge_e7_callhome_config_meta_json(s->e7, mac, jbuf, sizeof(jbuf));
        if (jn < 0) {
            static const char body[] =
                "{\"error\":\"NO_CONFIG\","
                "\"hint\":\"POST …/config/capture first (OPEN session required);"
                " then poll …/commands/{cmd_id} until status=ok\"}";
            if (build_response(out, out_cap, 404, "Not Found",
                               "application/json", body, sizeof(body) - 1,
                               out_len) != 0) {
                return -1;
            }
            note_response(metrics, 404);
            return 1;
        }
        if (build_response(out, out_cap, 200, "OK", "application/json", jbuf,
                           (size_t)jn, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 200);
        return 1;
    }

    /* GET config/onts — provisioned inventory */
    if (kind == 10 && strcmp(s->method, "GET") == 0) {
        static char invbuf[262144];
        jn = edge_e7_callhome_config_onts_json(s->e7, mac, NULL, invbuf,
                                               sizeof(invbuf));
        if (jn < 0) {
            static const char body[] = "{\"error\":\"TOO_LARGE_OR_BAD\"}";
            if (build_response(out, out_cap, 500, "Internal Server Error",
                               "application/json", body, sizeof(body) - 1,
                               out_len) != 0) {
                return -1;
            }
            note_response(metrics, 500);
            return 1;
        }
        if (build_response(out, out_cap, 200, "OK", "application/json", invbuf,
                           (size_t)jn, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 200);
        return 1;
    }

    /* GET config/full — serve mirrored JSON file */
    if (kind == 11 && strcmp(s->method, "GET") == 0) {
        char fpath[576];
        FILE *fp;
        long fsz;
        char *fbuf;
        size_t nr;
        if (edge_e7_callhome_config_full_path(s->e7, mac, fpath,
                                              sizeof(fpath)) != 0) {
            static const char body[] =
                "{\"error\":\"NO_CONFIG\","
                "\"hint\":\"POST …/config/capture then poll until ok;"
                " files land under var/e7_config/{mac}/latest.json\"}";
            if (build_response(out, out_cap, 404, "Not Found",
                               "application/json", body, sizeof(body) - 1,
                               out_len) != 0) {
                return -1;
            }
            note_response(metrics, 404);
            return 1;
        }
        fp = fopen(fpath, "r");
        if (!fp) {
            static const char body[] =
                "{\"error\":\"NO_CONFIG\","
                "\"hint\":\"capture file missing; re-run config/capture\"}";
            if (build_response(out, out_cap, 404, "Not Found",
                               "application/json", body, sizeof(body) - 1,
                               out_len) != 0) {
                return -1;
            }
            note_response(metrics, 404);
            return 1;
        }
        if (fseek(fp, 0, SEEK_END) != 0) {
            fclose(fp);
            return -1;
        }
        fsz = ftell(fp);
        if (fsz < 0 || fsz > 8 * 1024 * 1024) {
            fclose(fp);
            static const char body[] = "{\"error\":\"TOO_LARGE\"}";
            if (build_response(out, out_cap, 413, "Payload Too Large",
                               "application/json", body, sizeof(body) - 1,
                               out_len) != 0) {
                return -1;
            }
            note_response(metrics, 413);
            return 1;
        }
        if (fseek(fp, 0, SEEK_SET) != 0) {
            fclose(fp);
            return -1;
        }
        fbuf = (char *)malloc((size_t)fsz + 1);
        if (!fbuf) {
            fclose(fp);
            return -1;
        }
        nr = fread(fbuf, 1, (size_t)fsz, fp);
        fclose(fp);
        if (nr != (size_t)fsz) {
            free(fbuf);
            return -1;
        }
        fbuf[fsz] = '\0';
        if (build_response(out, out_cap, 200, "OK", "application/json", fbuf,
                           (size_t)fsz, out_len) != 0) {
            free(fbuf);
            return -1;
        }
        free(fbuf);
        note_response(metrics, 200);
        return 1;
    }

    /* GET connection progress events + live sessions */
    if (kind == 7 && strcmp(s->method, "GET") == 0) {
        jn = edge_e7_callhome_events_json(s->e7, since_id, jbuf, sizeof(jbuf));
        if (jn < 0) {
            static const char body[] = "{\"error\":\"EVENTS_FAIL\"}";
            if (build_response(out, out_cap, 500, "Internal Server Error",
                               "application/json", body, sizeof(body) - 1,
                               out_len) != 0) {
                return -1;
            }
            note_response(metrics, 500);
            return 1;
        }
        if (build_response(out, out_cap, 200, "OK", "application/json", jbuf,
                           (size_t)jn, out_len) != 0) {
            return -1;
        }
        note_response(metrics, 200);
        return 1;
    }

    status = 405;
    reason = "Method Not Allowed";
    {
        static const char body[] = "{\"error\":\"METHOD\"}";
        if (build_response(out, out_cap, status, reason, "application/json",
                           body, sizeof(body) - 1, out_len) != 0) {
            return -1;
        }
    }
    note_response(metrics, status);
    return 1;
}

static int dispatch(edge_http1_serve_t *s, edge_metrics_t *metrics, char *out,
                    size_t out_cap, size_t *out_len)
{
    char json[3072];
    int jn;
    static const char ok_body[] = "ok\n";
    static const char nf_body[] = "not found\n";
    static const char ma_body[] = "method not allowed\n";
    size_t max_file;
    const char *pkg_suffix = NULL;
    int st;

    max_file = s->roots.max_file_bytes ? s->roots.max_file_bytes
                                       : EDGE_STATIC_MAX_FILE;

    /* Auth routes (P1.7c) */
    st = dispatch_lab_login(s, metrics, out, out_cap, out_len);
    if (st != 0) {
        return st;
    }
    st = dispatch_auth_me(s, metrics, out, out_cap, out_len);
    if (st != 0) {
        return st;
    }

    /* OpenAI-compatible proxy under /v1 (P1.8b) */
    if (s->plugins &&
        (strncmp(s->path, "/v1/", 4) == 0 || strcmp(s->path, "/v1") == 0)) {
        edge_plugin_t *pl = edge_plugin_host_match(s->plugins, s->path);
        edge_http_req_t preq;
        edge_http_res_t pres;
        uint8_t *pbody = NULL;
        size_t pcap = s->max_upstream_body ? s->max_upstream_body
                                           : (256u * 1024u);
        int pst;
        edge_principal_t lab_prin;

        if (!pl) {
            if (build_response(out, out_cap, 404, "Not Found", "application/json",
                               "{\"error\":\"NOT_FOUND\"}", 20, out_len) != 0) {
                return -1;
            }
            note_response(metrics, 404);
            return 1;
        }
        /* Auth: open mode → synthetic employee; else require principal */
        if (!auth_enforced(s)) {
            edge_principal_clear(&lab_prin);
            lab_prin.authenticated = 1;
            snprintf(lab_prin.sub, sizeof(lab_prin.sub), "lab");
            lab_prin.roles = EDGE_ROLE_EMPLOYEE;
            s->principal = lab_prin;
        } else {
            st = require_rbac(s, EDGE_RES_OPENAI, NULL, NULL, metrics, out,
                              out_cap, out_len);
            if (st != 0) {
                return st;
            }
        }
        if (pcap < 65536) {
            pcap = 65536;
        }
        pbody = (uint8_t *)malloc(pcap);
        if (!pbody) {
            return -1;
        }
        memset(&preq, 0, sizeof(preq));
        preq.method = s->method;
        preq.path = s->path;
        preq.principal = &s->principal;
        preq.inbound_slot = s->inbound_slot;
        {
            const char *body;
            size_t blen = 0;
            get_body(s, &body, &blen);
            preq.body = (const uint8_t *)body;
            preq.body_len = blen;
        }
        memset(&pres, 0, sizeof(pres));
        pres.body = pbody;
        pres.body_cap = pcap;
        pst = edge_plugin_host_dispatch_http(s->plugins, pl, &preq, &pres);
        if (pst == EDGE_PLUGIN_PENDING) {
            pst = edge_plugin_host_finish_pending_sync(
                s->plugins, s->allow_blocking_dns, s->max_upstream_body, &pres);
        }
        if (pres.status == 0) {
            pres.status = (pst == EDGE_PLUGIN_OK) ? 200 : 500;
        }
        {
            const char *ctype = pres.content_type[0] ? pres.content_type
                                                     : "application/json";
            const char *reason = pres.reason[0] ? pres.reason : NULL;
            if (build_response(out, out_cap, pres.status,
                               reason ? reason : "OK", ctype,
                               (const char *)pres.body, pres.body_len,
                               out_len) != 0) {
                free(pbody);
                return -1;
            }
        }
        note_response(metrics, pres.status);
        free(pbody);
        return 1;
    }

    /* WebSocket stream upgrade (P1.7b) */
    if (edge_ws_path_is_stream(s->path)) {
        st = dispatch_stream(s, metrics, out, out_cap, out_len);
        if (st != 0) {
            return st;
        }
    }

    /* State API first (supports GET/PUT/DELETE) */
    if (strncmp(s->path, "/api/v1/state/", 14) == 0) {
        st = dispatch_state(s, metrics, out, out_cap, out_len);
        if (st != 0) {
            return st;
        }
    }

    /* Lab diagnostics: memory hierarchy + CPU flame sampling */
    if (strncmp(s->path, "/api/v1/debug", 13) == 0 &&
        (s->path[13] == '\0' || s->path[13] == '/' || s->path[13] == '?')) {
        st = dispatch_debug(s, metrics, out, out_cap, out_len);
        if (st != 0) {
            return st;
        }
    }

    /* CPE telemetry proxy → ClickHouse batch (POST /api/v1/telemetry/events) */
    if (strncmp(s->path, "/api/v1/telemetry", 17) == 0 &&
        (s->path[17] == '\0' || s->path[17] == '/' || s->path[17] == '?')) {
        st = dispatch_telemetry(s, metrics, out, out_cap, out_len);
        if (st != 0) {
            return st;
        }
    }

    /* Certificate Authority + public CRL */
    if ((strncmp(s->path, "/api/v1/ca", 10) == 0 &&
         (s->path[10] == '\0' || s->path[10] == '/' || s->path[10] == '?')) ||
        strcmp(s->path, "/ca/crl.pem") == 0) {
        st = dispatch_ca(s, metrics, out, out_cap, out_len);
        if (st != 0) {
            return st;
        }
    }

    /* E7 Call Home REST (PR-5) */
    if (strncmp(s->path, "/api/v1/e7", 10) == 0 &&
        (s->path[10] == '\0' || s->path[10] == '/' || s->path[10] == '?')) {
        st = dispatch_e7(s, metrics, out, out_cap, out_len);
        if (st != 0) {
            return st;
        }
    }

    /* Fiber explain templates / render (employee) */
    if (strncmp(s->path, "/api/v1/explain", 15) == 0 &&
        (s->path[15] == '\0' || s->path[15] == '/' || s->path[15] == '?')) {
        st = dispatch_explain(s, metrics, out, out_cap, out_len);
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
        edge_metrics_memory_extra_t mem_extra;
        edge_metrics_t tmp;
        const edge_metrics_t *m = metrics;

        memset(&mem_extra, 0, sizeof(mem_extra));
        if (s->store) {
            mem_extra.have_state = 1;
            mem_extra.state_rss_bytes = edge_state_rss_bytes(s->store);
        }
        if (s->e7 && edge_e7_callhome_enabled(s->e7)) {
            uint32_t open_n = edge_e7_callhome_open_count(s->e7);
            size_t per = edge_e7_session_rss_estimate();
            mem_extra.have_e7 = 1;
            mem_extra.e7_sessions_open = open_n;
            mem_extra.e7_rss_estimate = (uint64_t)open_n * (uint64_t)per;
            /* max_sessions not exported; 0 means unknown in UI */
            mem_extra.e7_max_sessions = 0;
        }
        if (!m) {
            edge_metrics_init(&tmp);
            m = &tmp;
        }
        jn = edge_metrics_format_health_json_ex(m, &mem_extra, json,
                                                sizeof(json));
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
        st = require_rbac(s, EDGE_RES_PACKAGES, NULL, NULL, metrics, out,
                          out_cap, out_len);
        if (st != 0) {
            return st;
        }
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
    s->ws_upgrade = 0;
    s->ws_key[0] = '\0';
    s->request_id[0] = '\0';
    edge_principal_clear(&s->principal);
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

void edge_http1_serve_set_ws_hub(edge_http1_serve_t *s, edge_ws_hub_t *hub)
{
    if (s) {
        s->hub = hub;
    }
}

void edge_http1_serve_set_auth(edge_http1_serve_t *s, edge_auth_ctx_t *auth)
{
    if (s) {
        s->auth = auth;
    }
}

void edge_http1_serve_set_plugin_host(edge_http1_serve_t *s,
                                      edge_plugin_host_t *ph)
{
    if (s) {
        s->plugins = ph;
    }
}

void edge_http1_serve_set_e7(edge_http1_serve_t *s, edge_e7_callhome_t *e7)
{
    if (s) {
        s->e7 = e7;
    }
}

void edge_http1_serve_set_clickhouse(edge_http1_serve_t *s,
                                     edge_clickhouse_t *ch)
{
    if (s) {
        s->clickhouse = ch;
    }
}

void edge_http1_serve_set_telemetry_auth(edge_http1_serve_t *s,
                                         const char *username,
                                         const char *password)
{
    if (s) {
        s->telemetry_user = username;
        s->telemetry_password = password;
    }
}

void edge_http1_serve_set_ca(edge_http1_serve_t *s, edge_ca_t *ca)
{
    if (s) {
        s->ca = ca;
    }
}

void edge_http1_serve_set_outbound_policy(edge_http1_serve_t *s,
                                          int allow_blocking_dns,
                                          size_t max_upstream_body)
{
    if (s) {
        s->allow_blocking_dns = allow_blocking_dns;
        s->max_upstream_body = max_upstream_body;
    }
}

void edge_http1_serve_set_service_api_key(edge_http1_serve_t *s,
                                          const char *service_api_key)
{
    if (s) {
        s->service_api_key = service_api_key;
    }
}

int edge_http1_serve_took_ws_upgrade(const edge_http1_serve_t *s)
{
    return s && s->ws_upgrade;
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
            char rid[EDGE_WS_REQUEST_ID];
            char cookie_hdr[768];
            char cookie_val[EDGE_AUTH_COOKIE_MAX];
            s->headers_done = 1;
            headers_just_done = 1;
            mark_body_off(s);
            if (http1_get_header(s->h1, "Content-Length", clbuf,
                                 sizeof(clbuf)) == 0) {
                s->content_length = atol(clbuf);
            } else {
                s->content_length = 0;
            }
            if (http1_get_header(s->h1, "Sec-WebSocket-Key", s->ws_key,
                                 sizeof(s->ws_key)) != 0) {
                s->ws_key[0] = '\0';
            }
            if (http1_get_header(s->h1, "X-Request-Id", rid, sizeof(rid)) == 0 &&
                rid[0]) {
                snprintf(s->request_id, sizeof(s->request_id), "%s", rid);
            } else if (s->hub) {
                edge_ws_hub_mint_request_id(s->hub, NULL, s->request_id,
                                            sizeof(s->request_id));
            } else {
                snprintf(s->request_id, sizeof(s->request_id), "eh0");
            }
            edge_principal_clear(&s->principal);
            if (s->auth &&
                s->auth->mode == EDGE_AUTH_MODE_LAB_PASSWORD &&
                http1_get_header(s->h1, "Cookie", cookie_hdr,
                                 sizeof(cookie_hdr)) == 0 &&
                edge_auth_cookie_extract(cookie_hdr, cookie_val,
                                         sizeof(cookie_val)) == 0) {
                (void)edge_auth_session_verify(s->auth, cookie_val,
                                               &s->principal);
            } else if (s->auth &&
                       s->auth->mode == EDGE_AUTH_MODE_PROXY_HEADERS) {
                char xuser[EDGE_AUTH_SUB_MAX];
                char xroles[EDGE_AUTH_ROLES_CSV_MAX];
                char xts[32];
                char xsig[EDGE_AUTH_PROXY_SIG_MAX];
                xuser[0] = xroles[0] = xts[0] = xsig[0] = '\0';
                if (http1_get_header(s->h1, EDGE_AUTH_HDR_USER, xuser,
                                     sizeof(xuser)) == 0 &&
                    http1_get_header(s->h1, EDGE_AUTH_HDR_TS, xts,
                                     sizeof(xts)) == 0 &&
                    http1_get_header(s->h1, EDGE_AUTH_HDR_SIG, xsig,
                                     sizeof(xsig)) == 0) {
                    (void)http1_get_header(s->h1, EDGE_AUTH_HDR_ROLES, xroles,
                                           sizeof(xroles));
                    (void)edge_auth_proxy_verify(
                        s->auth, xuser, xroles[0] ? xroles : NULL, xts, xsig,
                        &s->principal);
                }
            }
            /* Service bearer → service_openai (independent of employee ladder) */
            if (!s->principal.authenticated && s->service_api_key &&
                s->service_api_key[0]) {
                char authz[384];
                if (http1_get_header(s->h1, "Authorization", authz,
                                     sizeof(authz)) == 0 &&
                    strncmp(authz, "Bearer ", 7) == 0) {
                    const char *tok = authz + 7;
                    if (strcmp(tok, s->service_api_key) == 0) {
                        s->principal.authenticated = 1;
                        snprintf(s->principal.sub, sizeof(s->principal.sub),
                                 "service");
                        s->principal.roles = EDGE_ROLE_SERVICE_OPENAI;
                    }
                }
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

    /* PUT/POST with body: wait until full body present in acc */
    if ((strcmp(s->method, "PUT") == 0 || strcmp(s->method, "POST") == 0) &&
        s->content_length > 0) {
        if (!wait_full_body(s)) {
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
