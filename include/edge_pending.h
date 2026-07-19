/**
 * @file edge_pending.h
 * @brief Fixed pending_table for HTTP PENDING outbound (P1.8a / ADR-008).
 *
 * Create-time allocation only. Host parks inbound conn slots here while
 * upstream I/O is in flight. No syscalls.
 */
#ifndef EDGE_PENDING_H
#define EDGE_PENDING_H

#include "edge_plugin.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGE_PENDING_DEFAULT_CAP 256
#define EDGE_PENDING_SUB_MAX     64

typedef struct {
    int             used;
    uint32_t        inbound_slot;
    uint64_t        outbound_id;
    edge_plugin_t  *plugin;
    uint64_t        user_tag;
    uint64_t        started_ms;
    uint32_t        timeout_ms;
    char            principal_sub[EDGE_PENDING_SUB_MAX];
} edge_pending_entry_t;

typedef struct edge_pending_table edge_pending_table_t;

edge_pending_table_t *edge_pending_create(size_t capacity); /* 0 → default */
void                  edge_pending_destroy(edge_pending_table_t *t);

size_t edge_pending_capacity(const edge_pending_table_t *t);
size_t edge_pending_count(const edge_pending_table_t *t);
int    edge_pending_full(const edge_pending_table_t *t);

/**
 * Insert a new pending entry. Fails if table full, inbound already pending,
 * or invalid args.
 * @return 0 ok, -1 error (full / duplicate inbound / bad args).
 */
int edge_pending_insert(edge_pending_table_t *t, uint32_t inbound_slot,
                        uint64_t outbound_id, edge_plugin_t *plugin,
                        uint64_t user_tag, uint32_t timeout_ms,
                        uint64_t started_ms, const char *principal_sub);

/** Find by outbound_id. @return entry or NULL. */
edge_pending_entry_t *edge_pending_find_outbound(edge_pending_table_t *t,
                                                 uint64_t outbound_id);
const edge_pending_entry_t *
edge_pending_find_outbound_const(const edge_pending_table_t *t,
                                 uint64_t outbound_id);

/** Find by inbound_slot. */
edge_pending_entry_t *edge_pending_find_inbound(edge_pending_table_t *t,
                                                uint32_t inbound_slot);

/** Clear entry (by pointer from find_*). No-op if NULL / not used. */
void edge_pending_release(edge_pending_table_t *t, edge_pending_entry_t *e);

/** Release by outbound_id. @return 0 if found, -1 if missing. */
int edge_pending_release_outbound(edge_pending_table_t *t, uint64_t outbound_id);

/** Release by inbound_slot (client disconnect cancel). */
int edge_pending_release_inbound(edge_pending_table_t *t, uint32_t inbound_slot);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_PENDING_H */
