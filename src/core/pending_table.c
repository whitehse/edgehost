/**
 * @file pending_table.c
 * @brief Fixed-capacity PENDING outbound table (P1.8a). Create-time alloc only.
 */

#include "edge_pending.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct edge_pending_table {
    edge_pending_entry_t *slots;
    size_t                cap;
    size_t                count;
};

edge_pending_table_t *edge_pending_create(size_t capacity)
{
    edge_pending_table_t *t;

    if (capacity == 0) {
        capacity = EDGE_PENDING_DEFAULT_CAP;
    }
    t = (edge_pending_table_t *)calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->slots = (edge_pending_entry_t *)calloc(capacity, sizeof(*t->slots));
    if (!t->slots) {
        free(t);
        return NULL;
    }
    t->cap = capacity;
    t->count = 0;
    return t;
}

void edge_pending_destroy(edge_pending_table_t *t)
{
    if (!t) {
        return;
    }
    free(t->slots);
    free(t);
}

size_t edge_pending_capacity(const edge_pending_table_t *t)
{
    return t ? t->cap : 0;
}

size_t edge_pending_count(const edge_pending_table_t *t)
{
    return t ? t->count : 0;
}

int edge_pending_full(const edge_pending_table_t *t)
{
    return t && t->count >= t->cap;
}

int edge_pending_insert(edge_pending_table_t *t, uint32_t inbound_slot,
                        uint64_t outbound_id, edge_plugin_t *plugin,
                        uint64_t user_tag, uint32_t timeout_ms,
                        uint64_t started_ms, const char *principal_sub)
{
    size_t i;
    edge_pending_entry_t *free_slot = NULL;

    if (!t || !plugin || outbound_id == 0) {
        return -1;
    }
    if (t->count >= t->cap) {
        return -1;
    }
    /* duplicate inbound or outbound? */
    for (i = 0; i < t->cap; i++) {
        if (!t->slots[i].used) {
            if (!free_slot) {
                free_slot = &t->slots[i];
            }
            continue;
        }
        if (t->slots[i].inbound_slot == inbound_slot) {
            return -1; /* nested PENDING */
        }
        if (t->slots[i].outbound_id == outbound_id) {
            return -1;
        }
    }
    if (!free_slot) {
        return -1;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = 1;
    free_slot->inbound_slot = inbound_slot;
    free_slot->outbound_id = outbound_id;
    free_slot->plugin = plugin;
    free_slot->user_tag = user_tag;
    free_slot->timeout_ms = timeout_ms;
    free_slot->started_ms = started_ms;
    if (principal_sub && principal_sub[0]) {
        snprintf(free_slot->principal_sub, sizeof(free_slot->principal_sub),
                 "%s", principal_sub);
    }
    t->count++;
    return 0;
}

edge_pending_entry_t *edge_pending_find_outbound(edge_pending_table_t *t,
                                                 uint64_t outbound_id)
{
    size_t i;
    if (!t || outbound_id == 0) {
        return NULL;
    }
    for (i = 0; i < t->cap; i++) {
        if (t->slots[i].used && t->slots[i].outbound_id == outbound_id) {
            return &t->slots[i];
        }
    }
    return NULL;
}

const edge_pending_entry_t *
edge_pending_find_outbound_const(const edge_pending_table_t *t,
                                 uint64_t outbound_id)
{
    return edge_pending_find_outbound((edge_pending_table_t *)t, outbound_id);
}

edge_pending_entry_t *edge_pending_find_inbound(edge_pending_table_t *t,
                                                uint32_t inbound_slot)
{
    size_t i;
    if (!t) {
        return NULL;
    }
    for (i = 0; i < t->cap; i++) {
        if (t->slots[i].used && t->slots[i].inbound_slot == inbound_slot) {
            return &t->slots[i];
        }
    }
    return NULL;
}

void edge_pending_release(edge_pending_table_t *t, edge_pending_entry_t *e)
{
    if (!t || !e || !e->used) {
        return;
    }
    memset(e, 0, sizeof(*e));
    if (t->count > 0) {
        t->count--;
    }
}

int edge_pending_release_outbound(edge_pending_table_t *t, uint64_t outbound_id)
{
    edge_pending_entry_t *e = edge_pending_find_outbound(t, outbound_id);
    if (!e) {
        return -1;
    }
    edge_pending_release(t, e);
    return 0;
}

int edge_pending_release_inbound(edge_pending_table_t *t, uint32_t inbound_slot)
{
    edge_pending_entry_t *e = edge_pending_find_inbound(t, inbound_slot);
    if (!e) {
        return -1;
    }
    edge_pending_release(t, e);
    return 0;
}
