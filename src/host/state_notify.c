/**
 * @file state_notify.c
 * @brief Unified state put/delete + WS STATE_CHANGED (K6 / PR-1).
 */

#include "edge_state_notify.h"

#include <string.h>

static void mint_rid(edge_ws_hub_t *hub, const char *prefer, char *out,
                     size_t out_sz)
{
    if (prefer && prefer[0]) {
        size_t n = strlen(prefer);
        if (n >= out_sz) {
            n = out_sz - 1;
        }
        memcpy(out, prefer, n);
        out[n] = '\0';
        return;
    }
    if (hub) {
        edge_ws_hub_mint_request_id(hub, NULL, out, out_sz);
    } else if (out_sz > 0) {
        out[0] = '\0';
    }
}

edge_state_err_t edge_state_put_and_notify(edge_state_store_t *st,
                                           edge_ws_hub_t *hub, const char *ns,
                                           const char *key, const char *value,
                                           size_t value_len,
                                           const char *request_id,
                                           int coalesce)
{
    edge_state_err_t er;
    char rid[EDGE_WS_REQUEST_ID];

    (void)coalesce; /* dirty-set coalesce deferred to Call Home tick */

    if (!st) {
        return EDGE_STATE_NOMEM;
    }
    er = edge_state_put(st, ns, key, value, value_len);
    if (er != EDGE_STATE_OK) {
        return er;
    }
    if (hub) {
        mint_rid(hub, request_id, rid, sizeof(rid));
        (void)edge_ws_hub_broadcast_state_changed(hub, ns, key, "put", value,
                                                  value_len, rid);
    }
    return EDGE_STATE_OK;
}

edge_state_err_t edge_state_delete_and_notify(edge_state_store_t *st,
                                              edge_ws_hub_t *hub, const char *ns,
                                              const char *key,
                                              const char *request_id,
                                              int coalesce)
{
    edge_state_err_t er;
    char rid[EDGE_WS_REQUEST_ID];

    (void)coalesce;

    if (!st) {
        return EDGE_STATE_NOMEM;
    }
    er = edge_state_delete(st, ns, key);
    if (er != EDGE_STATE_OK) {
        return er;
    }
    if (hub) {
        mint_rid(hub, request_id, rid, sizeof(rid));
        (void)edge_ws_hub_broadcast_state_changed(hub, ns, key, "delete", NULL,
                                                  0, rid);
    }
    return EDGE_STATE_OK;
}
