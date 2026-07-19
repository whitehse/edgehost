/**
 * @file edge_plugin.h
 * @brief Plugin ABI: HTTP / SESSION kinds, PENDING, host API v0 (P1.8a / ADR-008).
 *
 * Plugins are statically linked. All I/O goes through edge_host_api_t —
 * plugins never open sockets or call malloc for wire buffers.
 */
#ifndef EDGE_PLUGIN_H
#define EDGE_PLUGIN_H

#include "edge_auth.h"

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct edge_plugin     edge_plugin_t;
typedef struct edge_host_api   edge_host_api_t;
typedef struct edge_plugin_vtbl edge_plugin_vtbl_t;

typedef enum {
    EDGE_PLUGIN_KIND_HTTP    = 1, /* request handler; may return PENDING */
    EDGE_PLUGIN_KIND_SESSION = 2  /* long-lived SM: feed / next_event */
} edge_plugin_kind_t;

typedef enum {
    EDGE_PLUGIN_OK      = 0,  /* res fully filled; host may write now */
    EDGE_PLUGIN_PENDING = 1,  /* HTTP only: outbound in flight */
    EDGE_PLUGIN_ERR     = -1
} edge_plugin_status_t;

/** Inbound HTTP request (host-owned buffers valid until response written). */
typedef struct {
    const char    *method;
    const char    *path;       /* path only, no scheme/host */
    const char    *query;      /* may be NULL or "" */
    const char *const *hdr_names;
    const char *const *hdr_values;
    size_t         n_headers;
    const uint8_t *body;
    size_t         body_len;
    const edge_principal_t *principal; /* may be NULL */
    uint32_t       inbound_slot;       /* host conn slot for pending_table */
} edge_http_req_t;

/** Inbound HTTP response scratch (host provides body buffer). */
typedef struct {
    int    status;              /* e.g. 200 */
    char   reason[64];          /* optional; default from status if empty */
    char   content_type[96];    /* default application/json if empty */
    uint8_t *body;              /* host buffer */
    size_t  body_len;
    size_t  body_cap;
} edge_http_res_t;

/** Outbound client request (plugin → host). Host copies before return. */
typedef struct {
    const char *method;
    const char *url;            /* absolute URL preferred */
    const char *host;           /* optional SNI/Host when url is path-style */
    const char *addr_override;  /* optional dotted IP; skip DNS */
    const char *const *hdr_names;
    const char *const *hdr_values;
    size_t n_headers;
    const uint8_t *body;
    size_t body_len;
    uint32_t timeout_ms;        /* 0 = default */
    uint64_t user_tag;          /* echoed in result */
} edge_http_client_req_t;

/** Outbound completion (host → plugin on_http_complete). */
typedef struct {
    uint64_t       outbound_id;
    uint64_t       user_tag;
    int            transport_err; /* 0 = HTTP response; else errno-class */
    int            status;        /* HTTP status when transport_err == 0 */
    const char *const *hdr_names;
    const char *const *hdr_values;
    size_t         n_headers;
    const uint8_t *body;
    size_t         body_len;
} edge_http_client_result_t;

struct edge_plugin_vtbl {
    const char        *name;
    const char        *version;
    edge_plugin_kind_t kind;

    int  (*init)(edge_plugin_t *self, const edge_host_api_t *host,
                 const void *cfg_opaque);
    void (*shutdown)(edge_plugin_t *self);
    int  (*on_config_reload)(edge_plugin_t *self, const void *cfg_opaque);

    /* KIND_HTTP */
    int (*on_http)(edge_plugin_t *self, const edge_http_req_t *req,
                   edge_http_res_t *res);
    int (*on_http_complete)(edge_plugin_t *self,
                             const edge_http_client_result_t *upstream,
                             edge_http_res_t *res);

    /* KIND_SESSION */
    int (*feed)(edge_plugin_t *self, const uint8_t *data, size_t len);
    int (*next_event)(edge_plugin_t *self, void *ev_out); /* opaque for now */

    int (*on_tick)(edge_plugin_t *self, uint64_t mono_ms);
};

struct edge_plugin {
    const edge_plugin_vtbl_t *vtbl;
    void                     *user_data;
    const edge_host_api_t    *host; /* set by host on init */
};

/** Host API v0 — enough for state + pending outbound; expand later. */
struct edge_host_api {
    void *ctx;

    int (*state_get)(void *ctx, const char *ns, const char *key, void *out,
                     size_t *inout_len);
    int (*state_put)(void *ctx, const char *ns, const char *key, const void *val,
                     size_t len);
    int (*emit_ws_broadcast)(void *ctx, const char *topic, const void *json,
                             size_t len);
    int (*log)(void *ctx, int level, const char *fmt, ...);

    /**
     * Schedule outbound HTTPS/HTTP. Only valid from on_http (or SESSION feed).
     * On success sets *out_id and expects later on_http_complete.
     * @return 0 ok, -1 full/invalid (plugin should map to 429/500).
     */
    int (*http_client_request)(void *ctx, const edge_http_client_req_t *req,
                               uint64_t *out_id);

    int (*schedule_timer)(void *ctx, uint64_t delay_ms, uint64_t *out_timer_id);
    int (*cancel_timer)(void *ctx, uint64_t timer_id);
};

/** Log levels for host->log */
enum {
    EDGE_LOG_ERROR = 0,
    EDGE_LOG_WARN  = 1,
    EDGE_LOG_INFO  = 2,
    EDGE_LOG_DEBUG = 3
};

/** Helper: write body into res if it fits. */
static inline int edge_http_res_set_body(edge_http_res_t *res, const void *data,
                                         size_t len)
{
    if (!res || !res->body || len > res->body_cap) {
        return -1;
    }
    if (len && data) {
        size_t i;
        const uint8_t *p = (const uint8_t *)data;
        for (i = 0; i < len; i++) {
            res->body[i] = p[i];
        }
    }
    res->body_len = len;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* EDGE_PLUGIN_H */
