/**
 * @file edge_ws.h
 * @brief WebSocket stream helpers + STATE_CHANGED fan-out hub (P1.7b).
 *
 * Host-side only (may allocate). Handshake uses RFC 6455 accept key;
 * framing is shaggy `websocket_*`.
 */
#ifndef EDGE_WS_H
#define EDGE_WS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGE_WS_KEY_MAX      64
#define EDGE_WS_ACCEPT_MAX   64
#define EDGE_WS_REQUEST_ID   48
#define EDGE_WS_MSG_MAX      (4096 + 512)
/* Per-subscriber pending STATE_CHANGED queue (bounded; drop oldest). */
#define EDGE_WS_PENDING_MAX  32

/**
 * Compute Sec-WebSocket-Accept from Sec-WebSocket-Key (RFC 6455).
 * @return 0 ok, -1 error.
 */
int edge_ws_accept_key(const char *sec_ws_key, char *out, size_t out_sz);

/**
 * Build HTTP/1.1 101 Switching Protocols response.
 * @return 0 ok, -1 error.
 */
int edge_ws_build_101(const char *sec_ws_key, char *out, size_t out_cap,
                      size_t *out_len);

/**
 * Format STATE_CHANGED JSON payload (NUL-terminated in out).
 * @p value may be NULL for delete (emits "value":null).
 * @return bytes written excl NUL, or -1.
 */
int edge_ws_format_state_changed(char *out, size_t out_cap, const char *ns,
                                 const char *key, const char *op,
                                 const char *value, size_t value_len,
                                 const char *request_id);

/** Path is /api/v1/stream or /api/v1/stream?... (topics optional). */
int edge_ws_path_is_stream(const char *path);

typedef struct edge_ws_hub edge_ws_hub_t;

edge_ws_hub_t *edge_ws_hub_create(size_t max_subs);
void           edge_ws_hub_destroy(edge_ws_hub_t *h);

/**
 * Subscribe connection slot (0..max_subs-1). Idempotent.
 * @return 0 ok, -1 bad slot.
 */
int edge_ws_hub_subscribe(edge_ws_hub_t *h, int conn_slot);
void edge_ws_hub_unsubscribe(edge_ws_hub_t *h, int conn_slot);
int  edge_ws_hub_is_subscribed(const edge_ws_hub_t *h, int conn_slot);

/**
 * Mint request_id (monotonic counter, hex) if @p prefer is NULL/empty.
 */
void edge_ws_hub_mint_request_id(edge_ws_hub_t *h, const char *prefer,
                                 char *out, size_t out_sz);

/**
 * Format STATE_CHANGED and enqueue on every active subscriber.
 * Drops oldest pending on a full sub queue.
 * If full-value format fails (too large for EDGE_WS_MSG_MAX), retries with a
 * compact {"truncated":true} value so clients can refetch; increments format-fail
 * counter.
 * @return number of subscribers that received a copy.
 */
int edge_ws_hub_broadcast_state_changed(edge_ws_hub_t *h, const char *ns,
                                        const char *key, const char *op,
                                        const char *value, size_t value_len,
                                        const char *request_id);

/**
 * Pop one pending text payload for @p conn_slot.
 * @return 1 if message copied, 0 if empty, -1 error.
 */
int edge_ws_hub_take_pending(edge_ws_hub_t *h, int conn_slot, char *out,
                             size_t out_cap, size_t *out_len);

size_t edge_ws_hub_subscriber_count(const edge_ws_hub_t *h);

/** Times full-value format failed and compact fallback was used (or both failed). */
uint64_t edge_ws_hub_format_fail_count(const edge_ws_hub_t *h);

/** Times oldest pending was dropped due to EDGE_WS_PENDING_MAX. */
uint64_t edge_ws_hub_drop_oldest_count(const edge_ws_hub_t *h);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_WS_H */
