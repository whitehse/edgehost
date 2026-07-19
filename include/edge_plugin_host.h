/**
 * @file edge_plugin_host.h
 * @brief Host-side plugin registry + host API v0 + PENDING dispatch (P1.8a).
 */
#ifndef EDGE_PLUGIN_HOST_H
#define EDGE_PLUGIN_HOST_H

#include "edge_pending.h"
#include "edge_plugin.h"
#include "edge_state.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGE_PLUGIN_HOST_MAX_PLUGINS 16
#define EDGE_PLUGIN_HOST_MAX_ROUTES  32

typedef struct edge_plugin_host edge_plugin_host_t;

typedef struct {
    size_t max_pending; /* 0 → EDGE_PENDING_DEFAULT_CAP */
    edge_state_store_t *state; /* not owned; may be NULL */
} edge_plugin_host_config_t;

edge_plugin_host_t *edge_plugin_host_create(const edge_plugin_host_config_t *cfg);
void                edge_plugin_host_destroy(edge_plugin_host_t *h);

const edge_host_api_t *edge_plugin_host_api(edge_plugin_host_t *h);
edge_pending_table_t  *edge_plugin_host_pending(edge_plugin_host_t *h);

/**
 * Register plugin and call vtbl->init. Plugin must outlive host (static).
 * @return 0 ok, -1 full/init failed.
 */
int edge_plugin_host_register(edge_plugin_host_t *h, edge_plugin_t *plugin,
                              const void *cfg_opaque);

/**
 * Map URL path prefix to plugin (longest-prefix match later; first match v1).
 * @return 0 ok, -1 full.
 */
int edge_plugin_host_add_route(edge_plugin_host_t *h, const char *path_prefix,
                               edge_plugin_t *plugin);

/** Find plugin for path or NULL. */
edge_plugin_t *edge_plugin_host_match(edge_plugin_host_t *h, const char *path);

/**
 * Dispatch KIND_HTTP on_http for matched route (or explicit plugin).
 * On PENDING: pending_table has entry; host must not write response yet.
 * On OK: res filled.
 * On ERR: res may contain status if plugin set it.
 *
 * @return EDGE_PLUGIN_OK | PENDING | ERR
 */
int edge_plugin_host_dispatch_http(edge_plugin_host_t *h, edge_plugin_t *plugin,
                                   const edge_http_req_t *req,
                                   edge_http_res_t *res);

/**
 * Complete a pending outbound (real I/O or test inject).
 * Calls plugin->on_http_complete; must not return PENDING (host coerces ERR).
 * Releases pending slot on return.
 * @return EDGE_PLUGIN_OK | ERR
 */
int edge_plugin_host_complete_outbound(edge_plugin_host_t *h,
                                       const edge_http_client_result_t *upstream,
                                       edge_http_res_t *res);

/**
 * Client disconnect while pending: free slot without on_http_complete.
 * @return 0 if a slot was freed, -1 if none.
 */
int edge_plugin_host_cancel_inbound(edge_plugin_host_t *h, uint32_t inbound_slot);

/**
 * Last outbound request accepted by http_client_request (for test harnesses).
 * Valid until next request. Returns NULL if none.
 */
const edge_http_client_req_t *
edge_plugin_host_last_client_req(const edge_plugin_host_t *h);

uint64_t edge_plugin_host_last_outbound_id(const edge_plugin_host_t *h);

/**
 * After dispatch returned PENDING: run host outbound client on last request,
 * then on_http_complete. Phase-1 blocking complete (P1.8b).
 * @return EDGE_PLUGIN_OK | ERR
 */
int edge_plugin_host_finish_pending_sync(edge_plugin_host_t *h,
                                         int allow_blocking_dns,
                                         size_t max_upstream_body,
                                         edge_http_res_t *res);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_PLUGIN_HOST_H */
