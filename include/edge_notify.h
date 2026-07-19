/**
 * @file edge_notify.h
 * @brief Postgres NOTIFY payload apply → state + WS (P1.12).
 *
 * Enforced schema (JSON object):
 *   {"ns":"...","key":"...","op":"put"|"delete","value":...}
 * value required for put; ignored/null for delete.
 * Optional request_id string.
 */
#ifndef EDGE_NOTIFY_H
#define EDGE_NOTIFY_H

#include "edge_state.h"
#include "edge_ws.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGE_NOTIFY_MAX_PAYLOAD 8000

typedef enum {
    EDGE_NOTIFY_OK = 0,
    EDGE_NOTIFY_BAD_JSON,
    EDGE_NOTIFY_BAD_SCHEMA,
    EDGE_NOTIFY_STATE_ERR,
    EDGE_NOTIFY_TOO_LARGE
} edge_notify_err_t;

const char *edge_notify_err_name(edge_notify_err_t e);

/**
 * Validate and apply one NOTIFY payload.
 * On put/delete success, broadcasts STATE_CHANGED if @p hub non-NULL.
 * @return EDGE_NOTIFY_OK or error.
 */
edge_notify_err_t edge_notify_apply(edge_state_store_t *st, edge_ws_hub_t *hub,
                                    const char *json, size_t json_len);

/**
 * Extract JSON string field "name" into out (NUL-terminated).
 * @return 0 ok, -1 missing/invalid.
 */
int edge_notify_json_string_field(const char *json, size_t json_len,
                                  const char *name, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_NOTIFY_H */
