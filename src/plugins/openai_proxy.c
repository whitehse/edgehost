/**
 * @file openai_proxy.c
 * @brief OpenAI-compatible /v1 proxy HTTP plugin with PENDING (P1.8b).
 */

#include "edge_openai_proxy.h"

#include "edge_auth.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    edge_openai_proxy_config_t cfg;
    char                       api_key[EDGE_OPENAI_KEY_MAX];
    /* naive rate limit: sliding window of timestamps per principal hash */
    uint64_t hits[64];
    int      hit_n;
    time_t   window_start;
    /* concurrent per principal: simple global count for v1 */
    int      in_flight;
} openai_data_t;

void edge_openai_proxy_config_defaults(edge_openai_proxy_config_t *c)
{
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->enabled = 0;
    snprintf(c->upstream, sizeof(c->upstream), "https://api.openai.com");
    snprintf(c->api_key_env, sizeof(c->api_key_env), "OPENAI_API_KEY");
    snprintf(c->service_api_key_env, sizeof(c->service_api_key_env),
             "EDGEHOST_OPENAI_SERVICE_KEY");
    snprintf(c->path_prefix, sizeof(c->path_prefix), "/v1");
    c->timeout_ms = 60000;
    c->rate_limit_rpm = 60;
    c->max_concurrent_per_principal = 4;
}

static int route_allowed(const char *method, const char *path)
{
    if (!method || !path) {
        return 0;
    }
    if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/v1/chat/completions") == 0 ||
            strncmp(path, "/v1/chat/completions?", 21) == 0) {
            return 1;
        }
        if (strcmp(path, "/v1/responses") == 0 ||
            strncmp(path, "/v1/responses?", 14) == 0) {
            return 1;
        }
    }
    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/v1/models") == 0 ||
            strncmp(path, "/v1/models?", 11) == 0) {
            return 1;
        }
    }
    return 0;
}

static int body_looks_ok(const uint8_t *body, size_t len, const char *method)
{
    /* GET needs no body */
    if (strcmp(method, "GET") == 0) {
        return 1;
    }
    if (!body || len < 2) {
        return 0;
    }
    /* must be JSON object and mention model for chat */
    {
        size_t i = 0;
        while (i < len && (body[i] == ' ' || body[i] == '\n' || body[i] == '\r' ||
                           body[i] == '\t')) {
            i++;
        }
        if (i >= len || body[i] != '{') {
            return 0;
        }
    }
    /* heuristic: require "model" key somewhere */
    {
        char tmp[96];
        size_t n = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
        memcpy(tmp, body, n);
        tmp[n] = '\0';
        if (strstr(tmp, "\"model\"") == NULL && len < sizeof(tmp)) {
            return 0;
        }
        if (len >= sizeof(tmp)) {
            /* large body: scan for model */
            size_t i;
            for (i = 0; i + 7 < len; i++) {
                if (memcmp(body + i, "\"model\"", 7) == 0) {
                    return 1;
                }
            }
            return 0;
        }
    }
    return 1;
}

static int rate_limit_ok(openai_data_t *d)
{
    time_t now = time(NULL);
    if (d->cfg.rate_limit_rpm == 0) {
        return 1;
    }
    if (d->window_start == 0 || now - d->window_start >= 60) {
        d->window_start = now;
        d->hit_n = 0;
    }
    if ((uint32_t)d->hit_n >= d->cfg.rate_limit_rpm) {
        return 0;
    }
    d->hit_n++;
    return 1;
}

static int principal_allowed(const edge_principal_t *p)
{
    if (!p || !p->authenticated) {
        return 0;
    }
    return edge_auth_role_has(p->roles, EDGE_ROLE_EMPLOYEE) ||
           edge_auth_role_has(p->roles, EDGE_ROLE_EMPLOYEE_ADMIN) ||
           edge_auth_role_has(p->roles, EDGE_ROLE_SERVICE_OPENAI);
}

static int openai_init(edge_plugin_t *self, const edge_host_api_t *host,
                       const void *cfg_opaque)
{
    (void)host;
    (void)cfg_opaque;
    (void)self;
    return 0;
}

static void openai_shutdown(edge_plugin_t *self)
{
    if (self && self->user_data) {
        free(self->user_data);
        self->user_data = NULL;
    }
}

static int openai_on_http(edge_plugin_t *self, const edge_http_req_t *req,
                          edge_http_res_t *res)
{
    openai_data_t *d = (openai_data_t *)self->user_data;
    edge_http_client_req_t creq;
    uint64_t oid = 0;
    char url[EDGE_OPENAI_URL_MAX + 128];
    const char *hdr_names[4];
    const char *hdr_values[4];
    char auth_hdr[EDGE_OPENAI_KEY_MAX + 32];
    char ctype[] = "application/json";
    size_t n_hdr = 0;
    const char *up_path;

    if (!d || !d->cfg.enabled) {
        res->status = 503;
        (void)edge_http_res_set_body(res, "{\"error\":\"OPENAI_DISABLED\"}", 27);
        return EDGE_PLUGIN_ERR;
    }
    if (!principal_allowed(req->principal)) {
        res->status = 401;
        (void)edge_http_res_set_body(res, "{\"error\":\"UNAUTHORIZED\"}", 24);
        return EDGE_PLUGIN_ERR;
    }
    if (!route_allowed(req->method, req->path)) {
        res->status = 404;
        (void)edge_http_res_set_body(res, "{\"error\":\"NOT_FOUND\"}", 20);
        return EDGE_PLUGIN_ERR;
    }
    if (!body_looks_ok(req->body, req->body_len, req->method)) {
        res->status = 400;
        (void)edge_http_res_set_body(res, "{\"error\":\"BAD_BODY\"}", 19);
        return EDGE_PLUGIN_ERR;
    }
    if (!rate_limit_ok(d)) {
        res->status = 429;
        snprintf(res->reason, sizeof(res->reason), "Too Many Requests");
        (void)edge_http_res_set_body(res, "{\"error\":\"RATE_LIMITED\"}", 24);
        return EDGE_PLUGIN_ERR;
    }
    if (d->cfg.max_concurrent_per_principal > 0 &&
        (uint32_t)d->in_flight >= d->cfg.max_concurrent_per_principal) {
        res->status = 429;
        (void)edge_http_res_set_body(res, "{\"error\":\"PENDING_FULL\"}", 24);
        return EDGE_PLUGIN_ERR;
    }
    if (!d->api_key[0]) {
        res->status = 500;
        (void)edge_http_res_set_body(res, "{\"error\":\"NO_API_KEY\"}", 21);
        return EDGE_PLUGIN_ERR;
    }

    /* map /v1/... onto upstream base */
    up_path = req->path;
    if (strncmp(d->cfg.upstream + strlen(d->cfg.upstream) - 1, "/", 1) == 0) {
        /* base ends with / */
    }
    {
        size_t ulen = strlen(d->cfg.upstream);
        if (ulen > 0 && d->cfg.upstream[ulen - 1] == '/') {
            snprintf(url, sizeof(url), "%s%s", d->cfg.upstream,
                     up_path[0] == '/' ? up_path + 1 : up_path);
        } else {
            snprintf(url, sizeof(url), "%s%s", d->cfg.upstream, up_path);
        }
    }

    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", d->api_key);
    hdr_names[n_hdr] = "Authorization";
    hdr_values[n_hdr] = auth_hdr;
    n_hdr++;
    hdr_names[n_hdr] = "Content-Type";
    hdr_values[n_hdr] = ctype;
    n_hdr++;

    memset(&creq, 0, sizeof(creq));
    creq.method = req->method;
    creq.url = url;
    creq.host =
        d->cfg.upstream_host[0] ? d->cfg.upstream_host : NULL;
    creq.addr_override =
        d->cfg.upstream_addr[0] ? d->cfg.upstream_addr : NULL;
    creq.hdr_names = hdr_names;
    creq.hdr_values = hdr_values;
    creq.n_headers = n_hdr;
    creq.body = req->body;
    creq.body_len = req->body_len;
    creq.timeout_ms = d->cfg.timeout_ms;
    creq.user_tag = req->inbound_slot;

    if (self->host->http_client_request(self->host->ctx, &creq, &oid) != 0) {
        res->status = 429;
        (void)edge_http_res_set_body(res, "{\"error\":\"PENDING_FULL\"}", 24);
        return EDGE_PLUGIN_ERR;
    }
    d->in_flight++;
    return EDGE_PLUGIN_PENDING;
}

static int openai_on_http_complete(edge_plugin_t *self,
                                   const edge_http_client_result_t *up,
                                   edge_http_res_t *res)
{
    openai_data_t *d = (openai_data_t *)self->user_data;
    if (d && d->in_flight > 0) {
        d->in_flight--;
    }

    if (up->transport_err != 0) {
        if (up->transport_err == ETIMEDOUT) {
            res->status = 504;
            snprintf(res->reason, sizeof(res->reason), "Gateway Timeout");
            (void)edge_http_res_set_body(
                res, "{\"error\":\"UPSTREAM_TIMEOUT\"}", 28);
            return EDGE_PLUGIN_ERR;
        }
        res->status = 502;
        snprintf(res->reason, sizeof(res->reason), "Bad Gateway");
        (void)edge_http_res_set_body(res, "{\"error\":\"UPSTREAM_CONNECT\"}", 27);
        return EDGE_PLUGIN_ERR;
    }

    /* pass through upstream status + body (buffered) */
    res->status = up->status;
    snprintf(res->content_type, sizeof(res->content_type), "application/json");
    if (up->body && up->body_len > 0) {
        if (edge_http_res_set_body(res, up->body, up->body_len) != 0) {
            res->status = 502;
            (void)edge_http_res_set_body(res, "{\"error\":\"UPSTREAM_TOO_LARGE\"}",
                                         29);
            return EDGE_PLUGIN_ERR;
        }
    } else {
        (void)edge_http_res_set_body(res, "{}", 2);
    }
    return EDGE_PLUGIN_OK;
}

static const edge_plugin_vtbl_t openai_vtbl = {
    .name = "openai_proxy",
    .version = "0.1",
    .kind = EDGE_PLUGIN_KIND_HTTP,
    .init = openai_init,
    .shutdown = openai_shutdown,
    .on_config_reload = NULL,
    .on_http = openai_on_http,
    .on_http_complete = openai_on_http_complete,
    .feed = NULL,
    .next_event = NULL,
    .on_tick = NULL,
};

const edge_plugin_vtbl_t *edge_openai_proxy_vtbl(void)
{
    return &openai_vtbl;
}

int edge_openai_proxy_init_plugin(edge_plugin_t *plugin,
                                  edge_openai_proxy_config_t *cfg_storage,
                                  const edge_openai_proxy_config_t *cfg)
{
    openai_data_t *d;
    const char *env_name;
    const char *key;

    if (!plugin || !cfg_storage) {
        return -1;
    }
    if (cfg) {
        *cfg_storage = *cfg;
    } else {
        edge_openai_proxy_config_defaults(cfg_storage);
    }

    d = (openai_data_t *)calloc(1, sizeof(*d));
    if (!d) {
        return -1;
    }
    d->cfg = *cfg_storage;
    env_name = d->cfg.api_key_env[0] ? d->cfg.api_key_env : "OPENAI_API_KEY";
    key = getenv(env_name);
    if (d->cfg.enabled) {
        if (!key || !key[0]) {
            free(d);
            return -1;
        }
        if (strlen(key) >= sizeof(d->api_key)) {
            free(d);
            return -1;
        }
        snprintf(d->api_key, sizeof(d->api_key), "%s", key);
    } else if (key && key[0] && strlen(key) < sizeof(d->api_key)) {
        snprintf(d->api_key, sizeof(d->api_key), "%s", key);
    }

    memset(plugin, 0, sizeof(*plugin));
    plugin->vtbl = &openai_vtbl;
    plugin->user_data = d;
    return 0;
}
