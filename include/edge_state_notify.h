/**
 * @file edge_state_notify.h
 * @brief Unified state put/delete + optional WS STATE_CHANGED fan-out (K6 / PR-1).
 *
 * Single choke point so REST, NOTIFY apply, plugin host API, and future Call Home
 * producers do not double-broadcast.
 */
#ifndef EDGE_STATE_NOTIFY_H
#define EDGE_STATE_NOTIFY_H

#include "edge_state.h"
#include "edge_ws.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Call edge_state_put; on EDGE_STATE_OK, if @p hub is non-NULL, broadcast
 * STATE_CHANGED (op "put").
 *
 * @param hub         NULL = put only (no fan-out)
 * @param request_id  optional; minted via hub when empty and hub non-NULL
 * @param coalesce    reserved for dirty-set path (E7); currently ignored —
 *                    all successful puts with a hub broadcast immediately
 * @return edge_state_put result (broadcast failure does not change return)
 */
edge_state_err_t edge_state_put_and_notify(edge_state_store_t *st,
                                           edge_ws_hub_t *hub, const char *ns,
                                           const char *key, const char *value,
                                           size_t value_len,
                                           const char *request_id,
                                           int coalesce);

/**
 * Call edge_state_delete; on EDGE_STATE_OK, if @p hub is non-NULL, broadcast
 * STATE_CHANGED (op "delete", value null). NOT_FOUND does not notify.
 */
edge_state_err_t edge_state_delete_and_notify(edge_state_store_t *st,
                                              edge_ws_hub_t *hub, const char *ns,
                                              const char *key,
                                              const char *request_id,
                                              int coalesce);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_STATE_NOTIFY_H */
