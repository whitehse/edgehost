/**
 * @file plugin_host.c
 * @brief Plugin registry, host API v0, PENDING dispatch (P1.8a).
 */

#include "edge_plugin_host.h"

#include "edge_outbound.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct edge_plugin_host {
    edge_state_store_t   *state; /* not owned */
    edge_pending_table_t *pending;
    edge_host_api_t       api;

    edge_plugin_t *plugins[EDGE_PLUGIN_HOST_MAX_PLUGINS];
    size_t         n_plugins;

    struct {
        char           prefix[128];
        edge_plugin_t *plugin;
    } routes[EDGE_PLUGIN_HOST_MAX_ROUTES];
    size_t n_routes;

    /* active dispatch context for http_client_request */
    edge_plugin_t *active_plugin;
    uint32_t       active_inbound_slot;
    int            active_in_on_http;
    int            client_req_started; /* set when http_client_request succeeds */

    uint64_t next_outbound_id; /* starts at 1 */

    /* last accepted client request (deep copy; lives past on_http return) */
    edge_http_client_req_t last_req;
    char                   last_method[16];
    char                   last_url[512];
    char                   last_host[256];
    char                   last_addr[64];
    uint8_t               *last_body;
    size_t                 last_body_len;
    size_t                 last_body_cap;
    char                   last_hdr_names[8][64];
    char                   last_hdr_values[8][384];
    const char            *last_hdr_name_ptrs[8];
    const char            *last_hdr_value_ptrs[8];
    size_t                 last_n_headers;
    uint64_t               last_outbound_id;
    int                    has_last_req;
};

/* --- host API implementations -------------------------------------------- */

static int api_state_get(void *ctx, const char *ns, const char *key, void *out,
                         size_t *inout_len)
{
    edge_plugin_host_t *h = (edge_plugin_host_t *)ctx;
    size_t cap;
    size_t got = 0;
    edge_state_err_t er;

    if (!h || !h->state || !inout_len) {
        return -1;
    }
    cap = *inout_len;
    er = edge_state_get(h->state, ns, key, (char *)out, cap, &got);
    if (er != EDGE_STATE_OK) {
        return -1;
    }
    *inout_len = got;
    return 0;
}

static int api_state_put(void *ctx, const char *ns, const char *key,
                         const void *val, size_t len)
{
    edge_plugin_host_t *h = (edge_plugin_host_t *)ctx;
    if (!h || !h->state) {
        return -1;
    }
    return edge_state_put(h->state, ns, key, (const char *)val, len) ==
                   EDGE_STATE_OK
               ? 0
               : -1;
}

static int api_emit_ws(void *ctx, const char *topic, const void *json,
                       size_t len)
{
    (void)ctx;
    (void)topic;
    (void)json;
    (void)len;
    /* WS hub wiring is host-loop concern; v0 no-op success */
    return 0;
}

static int api_log(void *ctx, int level, const char *fmt, ...)
{
    va_list ap;
    (void)ctx;
    (void)level;
    if (!fmt) {
        return -1;
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return 0;
}

static int store_last_req(edge_plugin_host_t *h, const edge_http_client_req_t *req)
{
    size_t blen;
    size_t i;
    size_t nh;

    if (!req || !req->method || !req->url) {
        return -1;
    }
    snprintf(h->last_method, sizeof(h->last_method), "%s", req->method);
    snprintf(h->last_url, sizeof(h->last_url), "%s", req->url);
    if (req->host) {
        snprintf(h->last_host, sizeof(h->last_host), "%s", req->host);
    } else {
        h->last_host[0] = '\0';
    }
    if (req->addr_override) {
        snprintf(h->last_addr, sizeof(h->last_addr), "%s", req->addr_override);
    } else {
        h->last_addr[0] = '\0';
    }
    blen = req->body_len;
    if (blen > 0 && req->body) {
        if (blen > h->last_body_cap) {
            uint8_t *nb = (uint8_t *)realloc(h->last_body, blen);
            if (!nb) {
                return -1;
            }
            h->last_body = nb;
            h->last_body_cap = blen;
        }
        memcpy(h->last_body, req->body, blen);
        h->last_body_len = blen;
    } else {
        h->last_body_len = 0;
    }
    nh = req->n_headers;
    if (nh > 8) {
        nh = 8;
    }
    h->last_n_headers = nh;
    for (i = 0; i < nh; i++) {
        snprintf(h->last_hdr_names[i], sizeof(h->last_hdr_names[i]), "%s",
                 req->hdr_names && req->hdr_names[i] ? req->hdr_names[i] : "");
        snprintf(h->last_hdr_values[i], sizeof(h->last_hdr_values[i]), "%s",
                 req->hdr_values && req->hdr_values[i] ? req->hdr_values[i]
                                                       : "");
        h->last_hdr_name_ptrs[i] = h->last_hdr_names[i];
        h->last_hdr_value_ptrs[i] = h->last_hdr_values[i];
    }
    memset(&h->last_req, 0, sizeof(h->last_req));
    h->last_req.method = h->last_method;
    h->last_req.url = h->last_url;
    h->last_req.host = h->last_host[0] ? h->last_host : NULL;
    h->last_req.addr_override = h->last_addr[0] ? h->last_addr : NULL;
    h->last_req.body = h->last_body_len ? h->last_body : NULL;
    h->last_req.body_len = h->last_body_len;
    h->last_req.timeout_ms = req->timeout_ms;
    h->last_req.user_tag = req->user_tag;
    h->last_req.hdr_names = h->last_hdr_name_ptrs;
    h->last_req.hdr_values = h->last_hdr_value_ptrs;
    h->last_req.n_headers = h->last_n_headers;
    h->has_last_req = 1;
    return 0;
}

static int api_http_client_request(void *ctx, const edge_http_client_req_t *req,
                                   uint64_t *out_id)
{
    edge_plugin_host_t *h = (edge_plugin_host_t *)ctx;
    uint64_t id;
    const char *sub = NULL;

    if (!h || !req || !out_id) {
        return -1;
    }
    if (!h->active_in_on_http || !h->active_plugin) {
        return -1; /* only from on_http */
    }
    if (h->client_req_started) {
        return -1; /* nested PENDING */
    }
    if (edge_pending_full(h->pending)) {
        return -1;
    }
    if (edge_pending_find_inbound(h->pending, h->active_inbound_slot)) {
        return -1;
    }
    if (store_last_req(h, req) != 0) {
        return -1;
    }

    id = h->next_outbound_id++;
    if (id == 0) {
        id = h->next_outbound_id++;
    }
    if (edge_pending_insert(h->pending, h->active_inbound_slot, id,
                            h->active_plugin, req->user_tag, req->timeout_ms, 0,
                            sub) != 0) {
        return -1;
    }
    h->client_req_started = 1;
    h->last_outbound_id = id;
    *out_id = id;
    return 0;
}

static int api_schedule_timer(void *ctx, uint64_t delay_ms, uint64_t *out_timer_id)
{
    (void)ctx;
    (void)delay_ms;
    (void)out_timer_id;
    return -1; /* P1.8a: stub */
}

static int api_cancel_timer(void *ctx, uint64_t timer_id)
{
    (void)ctx;
    (void)timer_id;
    return -1;
}

static void bind_api(edge_plugin_host_t *h)
{
    h->api.ctx = h;
    h->api.state_get = api_state_get;
    h->api.state_put = api_state_put;
    h->api.emit_ws_broadcast = api_emit_ws;
    h->api.log = api_log;
    h->api.http_client_request = api_http_client_request;
    h->api.schedule_timer = api_schedule_timer;
    h->api.cancel_timer = api_cancel_timer;
}

/* --- public -------------------------------------------------------------- */

edge_plugin_host_t *edge_plugin_host_create(const edge_plugin_host_config_t *cfg)
{
    edge_plugin_host_t *h;
    size_t cap = EDGE_PENDING_DEFAULT_CAP;

    h = (edge_plugin_host_t *)calloc(1, sizeof(*h));
    if (!h) {
        return NULL;
    }
    if (cfg) {
        if (cfg->max_pending > 0) {
            cap = cfg->max_pending;
        }
        h->state = cfg->state;
    }
    h->pending = edge_pending_create(cap);
    if (!h->pending) {
        free(h);
        return NULL;
    }
    h->next_outbound_id = 1;
    bind_api(h);
    return h;
}

void edge_plugin_host_destroy(edge_plugin_host_t *h)
{
    size_t i;
    if (!h) {
        return;
    }
    for (i = 0; i < h->n_plugins; i++) {
        edge_plugin_t *p = h->plugins[i];
        if (p && p->vtbl && p->vtbl->shutdown) {
            p->vtbl->shutdown(p);
        }
        if (p) {
            p->host = NULL;
        }
    }
    edge_pending_destroy(h->pending);
    free(h->last_body);
    free(h);
}

const edge_host_api_t *edge_plugin_host_api(edge_plugin_host_t *h)
{
    return h ? &h->api : NULL;
}

edge_pending_table_t *edge_plugin_host_pending(edge_plugin_host_t *h)
{
    return h ? h->pending : NULL;
}

int edge_plugin_host_register(edge_plugin_host_t *h, edge_plugin_t *plugin,
                              const void *cfg_opaque)
{
    if (!h || !plugin || !plugin->vtbl) {
        return -1;
    }
    if (h->n_plugins >= EDGE_PLUGIN_HOST_MAX_PLUGINS) {
        return -1;
    }
    plugin->host = &h->api;
    if (plugin->vtbl->init) {
        if (plugin->vtbl->init(plugin, &h->api, cfg_opaque) != 0) {
            plugin->host = NULL;
            return -1;
        }
    }
    h->plugins[h->n_plugins++] = plugin;
    return 0;
}

int edge_plugin_host_add_route(edge_plugin_host_t *h, const char *path_prefix,
                               edge_plugin_t *plugin)
{
    if (!h || !path_prefix || !path_prefix[0] || !plugin) {
        return -1;
    }
    if (h->n_routes >= EDGE_PLUGIN_HOST_MAX_ROUTES) {
        return -1;
    }
    if (strlen(path_prefix) >= sizeof(h->routes[0].prefix)) {
        return -1;
    }
    snprintf(h->routes[h->n_routes].prefix, sizeof(h->routes[0].prefix), "%s",
             path_prefix);
    h->routes[h->n_routes].plugin = plugin;
    h->n_routes++;
    return 0;
}

edge_plugin_t *edge_plugin_host_match(edge_plugin_host_t *h, const char *path)
{
    size_t i;
    edge_plugin_t *best = NULL;
    size_t best_len = 0;

    if (!h || !path) {
        return NULL;
    }
    for (i = 0; i < h->n_routes; i++) {
        size_t plen = strlen(h->routes[i].prefix);
        if (plen == 0) {
            continue;
        }
        if (strncmp(path, h->routes[i].prefix, plen) == 0) {
            /* require boundary: end or '/' after prefix unless prefix is "/" */
            if (path[plen] == '\0' || path[plen] == '/' ||
                h->routes[i].prefix[plen - 1] == '/') {
                if (plen >= best_len) {
                    best_len = plen;
                    best = h->routes[i].plugin;
                }
            }
        }
    }
    return best;
}

int edge_plugin_host_dispatch_http(edge_plugin_host_t *h, edge_plugin_t *plugin,
                                   const edge_http_req_t *req,
                                   edge_http_res_t *res)
{
    int st;

    if (!h || !plugin || !plugin->vtbl || !req || !res) {
        return EDGE_PLUGIN_ERR;
    }
    if (plugin->vtbl->kind != EDGE_PLUGIN_KIND_HTTP) {
        return EDGE_PLUGIN_ERR;
    }
    if (!plugin->vtbl->on_http) {
        return EDGE_PLUGIN_ERR;
    }
    if (edge_pending_find_inbound(h->pending, req->inbound_slot)) {
        /* already pending this slot */
        res->status = 500;
        snprintf(res->reason, sizeof(res->reason), "Internal Server Error");
        (void)edge_http_res_set_body(res, "{\"error\":\"NESTED_PENDING\"}", 26);
        return EDGE_PLUGIN_ERR;
    }

    h->active_plugin = plugin;
    h->active_inbound_slot = req->inbound_slot;
    h->active_in_on_http = 1;
    h->client_req_started = 0;

    st = plugin->vtbl->on_http(plugin, req, res);

    h->active_in_on_http = 0;
    h->active_plugin = NULL;

    if (st == EDGE_PLUGIN_PENDING) {
        if (!h->client_req_started) {
            /* plugin bug: PENDING without http_client_request */
            (void)edge_pending_release_inbound(h->pending, req->inbound_slot);
            res->status = 500;
            snprintf(res->reason, sizeof(res->reason), "Internal Server Error");
            (void)edge_http_res_set_body(res, "{\"error\":\"PENDING_WITHOUT_OUTBOUND\"}",
                                         35);
            return EDGE_PLUGIN_ERR;
        }
        return EDGE_PLUGIN_PENDING;
    }

    if (st == EDGE_PLUGIN_OK) {
        return EDGE_PLUGIN_OK;
    }

    if (res->status == 0) {
        res->status = 500;
        snprintf(res->reason, sizeof(res->reason), "Internal Server Error");
    }
    return EDGE_PLUGIN_ERR;
}

int edge_plugin_host_complete_outbound(edge_plugin_host_t *h,
                                       const edge_http_client_result_t *upstream,
                                       edge_http_res_t *res)
{
    edge_pending_entry_t *e;
    edge_plugin_t *plugin;
    int st;

    if (!h || !upstream || !res || upstream->outbound_id == 0) {
        return EDGE_PLUGIN_ERR;
    }
    e = edge_pending_find_outbound(h->pending, upstream->outbound_id);
    if (!e) {
        return EDGE_PLUGIN_ERR;
    }
    plugin = e->plugin;
    if (!plugin || !plugin->vtbl || !plugin->vtbl->on_http_complete) {
        edge_pending_release(h->pending, e);
        res->status = 500;
        (void)edge_http_res_set_body(res, "{\"error\":\"NO_COMPLETE_HANDLER\"}", 30);
        return EDGE_PLUGIN_ERR;
    }

    st = plugin->vtbl->on_http_complete(plugin, upstream, res);
    edge_pending_release(h->pending, e);

    if (st == EDGE_PLUGIN_PENDING) {
        /* forbidden: second hop not in v1 */
        res->status = 500;
        snprintf(res->reason, sizeof(res->reason), "Internal Server Error");
        (void)edge_http_res_set_body(res, "{\"error\":\"DOUBLE_PENDING\"}", 25);
        return EDGE_PLUGIN_ERR;
    }
    if (st == EDGE_PLUGIN_OK) {
        return EDGE_PLUGIN_OK;
    }
    if (res->status == 0) {
        res->status = 502;
        snprintf(res->reason, sizeof(res->reason), "Bad Gateway");
    }
    return EDGE_PLUGIN_ERR;
}

int edge_plugin_host_cancel_inbound(edge_plugin_host_t *h, uint32_t inbound_slot)
{
    if (!h) {
        return -1;
    }
    return edge_pending_release_inbound(h->pending, inbound_slot);
}

const edge_http_client_req_t *
edge_plugin_host_last_client_req(const edge_plugin_host_t *h)
{
    if (!h || !h->has_last_req) {
        return NULL;
    }
    return &h->last_req;
}

uint64_t edge_plugin_host_last_outbound_id(const edge_plugin_host_t *h)
{
    return h ? h->last_outbound_id : 0;
}

int edge_plugin_host_finish_pending_sync(edge_plugin_host_t *h,
                                         int allow_blocking_dns,
                                         size_t max_upstream_body,
                                         edge_http_res_t *res)
{
    edge_outbound_opts_t oopts;
    edge_http_client_result_t up;
    const edge_http_client_req_t *creq;
    uint8_t *ubuf = NULL;
    size_t ulen = 0;
    size_t cap;
    int rc;

    if (!h || !res) {
        return EDGE_PLUGIN_ERR;
    }
    creq = edge_plugin_host_last_client_req(h);
    if (!creq || h->last_outbound_id == 0) {
        return EDGE_PLUGIN_ERR;
    }
    edge_outbound_opts_defaults(&oopts);
    oopts.allow_blocking_dns = allow_blocking_dns;
    oopts.max_response_body =
        max_upstream_body ? max_upstream_body : oopts.max_response_body;
    if (creq->timeout_ms) {
        oopts.default_timeout_ms = creq->timeout_ms;
    }
    cap = oopts.max_response_body;
    if (cap < 4096) {
        cap = 4096;
    }
    ubuf = (uint8_t *)malloc(cap);
    if (!ubuf) {
        res->status = 500;
        (void)edge_http_res_set_body(res, "{\"error\":\"NOMEM\"}", 16);
        (void)edge_pending_release_outbound(h->pending, h->last_outbound_id);
        return EDGE_PLUGIN_ERR;
    }
    memset(&up, 0, sizeof(up));
    up.user_tag = creq->user_tag;
    if (edge_outbound_http_execute(creq, &oopts, &up, ubuf, cap, &ulen) != 0) {
        up.transport_err = EIO;
    }
    /* execute may zero the result struct; always restore correlation ids */
    up.outbound_id = h->last_outbound_id;
    up.user_tag = creq->user_tag;
    up.body = ubuf;
    up.body_len = ulen;
    rc = edge_plugin_host_complete_outbound(h, &up, res);
    free(ubuf);
    return rc;
}
