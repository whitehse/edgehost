/**
 * @file edge_http1_serve.h
 * @brief Shared shaggy HTTP/1 request → response (health, SPA, packages, state, WS, auth).
 */
#ifndef EDGE_HTTP1_SERVE_H
#define EDGE_HTTP1_SERVE_H

#include "edge_auth.h"
#include "edge_e7_callhome.h"
#include "edge_metrics.h"
#include "edge_plugin_host.h"
#include "edge_state.h"
#include "edge_ws.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct edge_http1_serve edge_http1_serve_t;

typedef struct {
    const char *spa_root;
    const char *packages_root;
    size_t      max_file_bytes;
} edge_http1_docroot_t;

edge_http1_serve_t *edge_http1_serve_create(void);
void                edge_http1_serve_destroy(edge_http1_serve_t *s);
void                edge_http1_serve_reset(edge_http1_serve_t *s);

void edge_http1_serve_set_docroots(edge_http1_serve_t *s,
                                   const edge_http1_docroot_t *roots);

/** Attach state store (not owned; may be NULL). */
void edge_http1_serve_set_state(edge_http1_serve_t *s, edge_state_store_t *st);

/**
 * Attach WS fan-out hub (not owned; may be NULL).
 * Successful PUT/DELETE enqueue STATE_CHANGED for all subscribers.
 */
void edge_http1_serve_set_ws_hub(edge_http1_serve_t *s, edge_ws_hub_t *hub);

/**
 * Attach auth context (not owned; may be NULL → treat as OPEN).
 */
void edge_http1_serve_set_auth(edge_http1_serve_t *s, edge_auth_ctx_t *auth);

/**
 * Attach plugin host for /v1 openai_proxy routes (not owned; may be NULL).
 */
void edge_http1_serve_set_plugin_host(edge_http1_serve_t *s,
                                      edge_plugin_host_t *ph);

/**
 * Attach E7 Call Home engine for /api/v1/e7/ (not owned; may be NULL -> 503).
 */
void edge_http1_serve_set_e7(edge_http1_serve_t *s, edge_e7_callhome_t *e7);

/** DNS / outbound policy for PENDING sync complete. */
void edge_http1_serve_set_outbound_policy(edge_http1_serve_t *s,
                                          int allow_blocking_dns,
                                          size_t max_upstream_body);

/**
 * Optional service API key for Authorization: Bearer → service_openai principal.
 * Not owned; pointer must outlive requests (or NULL).
 */
void edge_http1_serve_set_service_api_key(edge_http1_serve_t *s,
                                          const char *service_api_key);

/**
 * Feed inbound bytes.
 * @return 1 response ready (HTTP done or WS 101 handshake),
 *         0 need more, -1 hard error
 */
int edge_http1_serve_feed(edge_http1_serve_t *s, const uint8_t *data, size_t len,
                          edge_metrics_t *metrics, char *out, size_t out_cap,
                          size_t *out_len);

const char *edge_http1_serve_method(const edge_http1_serve_t *s);
const char *edge_http1_serve_path(const edge_http1_serve_t *s);

/**
 * After feed returns 1: true if response is 101 WS upgrade for /api/v1/stream.
 * Host should keep the connection and switch to WebSocket framing.
 */
int edge_http1_serve_took_ws_upgrade(const edge_http1_serve_t *s);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_HTTP1_SERVE_H */
