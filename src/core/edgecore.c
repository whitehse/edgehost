/**
 * @file edgecore.c
 * @brief edgecore: event ring, host buffers, config apply (P1.1–P1.3).
 *
 * No syscalls. Create-time allocation only for core object + event ring.
 * Post-create data buffers: NEED_ALLOC / NEED_REALLOC → host_alloc →
 * edgecore_provide_buffer (ADR-003). Config: pure validate + swap (ADR-005).
 */

#include "edgecore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EDGECORE_DEFAULT_Q 64u
#define EDGECORE_MAX_Q     256u

typedef enum {
    EDGE_BUF_SLOT_FREE = 0,
    EDGE_BUF_SLOT_PENDING,
    EDGE_BUF_SLOT_READY
} edge_buf_slot_state_t;

typedef struct {
    edge_buf_slot_state_t state;
    edge_buf_kind_t       kind;
    uint32_t              alloc_id;
    void                 *ptr;
    size_t                size;
    size_t                requested;
} edge_buf_slot_t;

struct edgecore {
    size_t         qsz;
    edge_event_t  *events;
    size_t         head;
    size_t         tail;
    size_t         cnt;
    uint64_t       dropped;

    edge_buf_slot_t bufs[EDGECORE_MAX_BUFS];
    uint32_t        next_alloc_id; /* 1..; skip 0 */

    edge_config_t   cfg;
    int             cfg_applied; /* 0 until first successful apply */
};

/* -------------------------------------------------------------------------- */

edgecore_config_t edgecore_default_config(void)
{
    edgecore_config_t c;
    memset(&c, 0, sizeof(c));
    c.event_queue_size = 0;
    return c;
}

edgecore_t *edgecore_create(void)
{
    edgecore_config_t c = edgecore_default_config();
    return edgecore_create_with_config(&c);
}

edgecore_t *edgecore_create_with_config(const edgecore_config_t *cfg)
{
    edgecore_t *core;
    size_t qsz;

    qsz = EDGECORE_DEFAULT_Q;
    if (cfg && cfg->event_queue_size > 0) {
        qsz = cfg->event_queue_size;
        if (qsz > EDGECORE_MAX_Q) {
            qsz = EDGECORE_MAX_Q;
        }
    }

    core = (edgecore_t *)calloc(1, sizeof(*core));
    if (!core) {
        return NULL;
    }

    core->events = (edge_event_t *)calloc(qsz, sizeof(edge_event_t));
    if (!core->events) {
        free(core);
        return NULL;
    }

    core->qsz = qsz;
    core->head = 0;
    core->tail = 0;
    core->cnt = 0;
    core->dropped = 0;
    core->next_alloc_id = 1;
    edge_config_defaults(&core->cfg);
    core->cfg_applied = 0;
    return core;
}

void edgecore_destroy(edgecore_t *core)
{
    if (!core) {
        return;
    }
    /* Host-owned data buffers are not freed here (ADR-003). */
    free(core->events);
    free(core);
}

/* -------------------------------------------------------------------------- */
/* Event ring                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * Enqueue. On full ring: replace oldest with QUEUE_OVERFLOW and drop the new
 * event (sibling ring overflow pattern). Returns 0 ok, -1 dropped.
 */
static int emit_event(edgecore_t *core, const edge_event_t *ev)
{
    if (core->cnt >= core->qsz) {
        edge_event_t overflow;
        memset(&overflow, 0, sizeof(overflow));
        overflow.type = EDGE_EVENT_QUEUE_OVERFLOW;
        snprintf(overflow.reason, sizeof(overflow.reason),
                 "event queue full (dropped new event)");
        core->events[core->head] = overflow;
        core->dropped++;
        return -1;
    }
    core->events[core->tail] = *ev;
    core->tail = (core->tail + 1) % core->qsz;
    core->cnt++;
    return 0;
}

int edgecore_next_event(edgecore_t *core, edge_event_t *ev)
{
    if (!core || !ev) {
        return -1;
    }
    if (core->cnt == 0) {
        memset(ev, 0, sizeof(*ev));
        ev->type = EDGE_EVENT_NONE;
        return 0;
    }

    *ev = core->events[core->head];
    core->head = (core->head + 1) % core->qsz;
    core->cnt--;
    return 1;
}

int edgecore_has_pending_events(const edgecore_t *core)
{
    return core && core->cnt > 0;
}

size_t edgecore_event_count(const edgecore_t *core)
{
    return core ? core->cnt : 0;
}

uint64_t edgecore_dropped_count(const edgecore_t *core)
{
    return core ? core->dropped : 0;
}

const char *edgecore_event_type_name(edge_event_type_t type)
{
    switch (type) {
    case EDGE_EVENT_NONE:            return "NONE";
    case EDGE_EVENT_NEED_ALLOC:      return "NEED_ALLOC";
    case EDGE_EVENT_NEED_REALLOC:    return "NEED_REALLOC";
    case EDGE_EVENT_WANT_SEND:       return "WANT_SEND";
    case EDGE_EVENT_HTTP_REQUEST:    return "HTTP_REQUEST";
    case EDGE_EVENT_STATE_CHANGED:   return "STATE_CHANGED";
    case EDGE_EVENT_WS_BROADCAST:    return "WS_BROADCAST";
    case EDGE_EVENT_CONFIG_APPLIED:  return "CONFIG_APPLIED";
    case EDGE_EVENT_CONFIG_REJECTED: return "CONFIG_REJECTED";
    case EDGE_EVENT_OUTBOUND_DONE:   return "OUTBOUND_DONE";
    case EDGE_EVENT_QUEUE_OVERFLOW:  return "QUEUE_OVERFLOW";
    default:                         return "UNKNOWN";
    }
}

const char *edgecore_buf_kind_name(edge_buf_kind_t kind)
{
    switch (kind) {
    case EDGE_BUF_NONE:      return "NONE";
    case EDGE_BUF_GENERIC:   return "GENERIC";
    case EDGE_BUF_CONN_RX:   return "CONN_RX";
    case EDGE_BUF_CONN_TX:   return "CONN_TX";
    case EDGE_BUF_HTTP_BODY: return "HTTP_BODY";
    case EDGE_BUF_STATE:     return "STATE";
    default:                 return "UNKNOWN";
    }
}

/* -------------------------------------------------------------------------- */
/* Buffer table                                                                */
/* -------------------------------------------------------------------------- */

static edge_buf_slot_t *find_slot_by_id(edgecore_t *core, uint32_t alloc_id)
{
    size_t i;

    if (!core || alloc_id == 0) {
        return NULL;
    }
    for (i = 0; i < EDGECORE_MAX_BUFS; i++) {
        if (core->bufs[i].state != EDGE_BUF_SLOT_FREE &&
            core->bufs[i].alloc_id == alloc_id) {
            return &core->bufs[i];
        }
    }
    return NULL;
}

static edge_buf_slot_t *find_free_slot(edgecore_t *core)
{
    size_t i;

    for (i = 0; i < EDGECORE_MAX_BUFS; i++) {
        if (core->bufs[i].state == EDGE_BUF_SLOT_FREE) {
            return &core->bufs[i];
        }
    }
    return NULL;
}

static uint32_t mint_alloc_id(edgecore_t *core)
{
    uint32_t id = core->next_alloc_id;

    if (id == 0) {
        id = 1;
    }
    core->next_alloc_id = id + 1;
    if (core->next_alloc_id == 0) {
        core->next_alloc_id = 1;
    }
    return id;
}

uint32_t edgecore_request_alloc(edgecore_t *core, edge_buf_kind_t kind,
                                size_t size)
{
    edge_buf_slot_t *slot;
    edge_event_t ev;
    uint32_t id;

    if (!core || size == 0 || kind == EDGE_BUF_NONE) {
        return 0;
    }

    slot = find_free_slot(core);
    if (!slot) {
        return 0;
    }

    id = mint_alloc_id(core);
    memset(slot, 0, sizeof(*slot));
    slot->state = EDGE_BUF_SLOT_PENDING;
    slot->kind = kind;
    slot->alloc_id = id;
    slot->requested = size;
    slot->ptr = NULL;
    slot->size = 0;

    memset(&ev, 0, sizeof(ev));
    ev.type = EDGE_EVENT_NEED_ALLOC;
    ev.alloc_id = id;
    ev.buf_kind = kind;
    ev.size = size;
    snprintf(ev.reason, sizeof(ev.reason), "NEED_ALLOC %s size=%zu",
             edgecore_buf_kind_name(kind), size);

    if (emit_event(core, &ev) != 0) {
        /* Queue full: roll back slot so host is not left with a stuck pending. */
        memset(slot, 0, sizeof(*slot));
        return 0;
    }
    return id;
}

int edgecore_request_realloc(edgecore_t *core, uint32_t alloc_id,
                             size_t new_size)
{
    edge_buf_slot_t *slot;
    edge_event_t ev;

    if (!core || alloc_id == 0 || new_size == 0) {
        return -1;
    }

    slot = find_slot_by_id(core, alloc_id);
    if (!slot || slot->state != EDGE_BUF_SLOT_READY) {
        return -1;
    }

    slot->state = EDGE_BUF_SLOT_PENDING;
    slot->requested = new_size;

    memset(&ev, 0, sizeof(ev));
    ev.type = EDGE_EVENT_NEED_REALLOC;
    ev.alloc_id = alloc_id;
    ev.buf_kind = slot->kind;
    ev.size = new_size;
    ev.old_ptr = slot->ptr;
    ev.old_size = slot->size;
    snprintf(ev.reason, sizeof(ev.reason), "NEED_REALLOC %s id=%u size=%zu",
             edgecore_buf_kind_name(slot->kind), alloc_id, new_size);

    if (emit_event(core, &ev) != 0) {
        /* Restore READY so buffer remains usable; drop the grow request. */
        slot->state = EDGE_BUF_SLOT_READY;
        slot->requested = slot->size;
        return -1;
    }
    return 0;
}

int edgecore_provide_buffer(edgecore_t *core, uint32_t alloc_id, void *ptr,
                            size_t size)
{
    edge_buf_slot_t *slot;

    if (!core || alloc_id == 0 || !ptr || size == 0) {
        return -1;
    }

    slot = find_slot_by_id(core, alloc_id);
    if (!slot || slot->state != EDGE_BUF_SLOT_PENDING) {
        return -1;
    }
    if (size < slot->requested) {
        return -1;
    }

    slot->ptr = ptr;
    slot->size = size;
    slot->state = EDGE_BUF_SLOT_READY;
    return 0;
}

int edgecore_detach_buffer(edgecore_t *core, uint32_t alloc_id, void **out_ptr,
                           size_t *out_size)
{
    edge_buf_slot_t *slot;

    if (!core || alloc_id == 0) {
        return -1;
    }

    slot = find_slot_by_id(core, alloc_id);
    if (!slot) {
        return -1;
    }

    if (out_ptr) {
        *out_ptr = slot->ptr;
    }
    if (out_size) {
        *out_size = slot->size;
    }
    memset(slot, 0, sizeof(*slot));
    return 0;
}

const void *edgecore_buffer_ptr(const edgecore_t *core, uint32_t alloc_id)
{
    size_t i;

    if (!core || alloc_id == 0) {
        return NULL;
    }
    for (i = 0; i < EDGECORE_MAX_BUFS; i++) {
        if (core->bufs[i].state == EDGE_BUF_SLOT_READY &&
            core->bufs[i].alloc_id == alloc_id) {
            return core->bufs[i].ptr;
        }
    }
    return NULL;
}

size_t edgecore_buffer_size(const edgecore_t *core, uint32_t alloc_id)
{
    size_t i;

    if (!core || alloc_id == 0) {
        return 0;
    }
    for (i = 0; i < EDGECORE_MAX_BUFS; i++) {
        if (core->bufs[i].state == EDGE_BUF_SLOT_READY &&
            core->bufs[i].alloc_id == alloc_id) {
            return core->bufs[i].size;
        }
    }
    return 0;
}

size_t edgecore_buffer_count(const edgecore_t *core)
{
    size_t i, n = 0;

    if (!core) {
        return 0;
    }
    for (i = 0; i < EDGECORE_MAX_BUFS; i++) {
        if (core->bufs[i].state == EDGE_BUF_SLOT_READY) {
            n++;
        }
    }
    return n;
}

/* -------------------------------------------------------------------------- */
/* Config apply (ADR-005)                                                      */
/* -------------------------------------------------------------------------- */

int edgecore_apply_config(edgecore_t *core, const edge_config_t *cfg)
{
    edge_event_t ev;
    char verr[96];
    edge_config_t next;

    if (!core || !cfg) {
        return -1;
    }

    memset(&ev, 0, sizeof(ev));
    if (edge_config_validate(cfg, verr, sizeof(verr)) != 0) {
        ev.type = EDGE_EVENT_CONFIG_REJECTED;
        snprintf(ev.reason, sizeof(ev.reason), "%s", verr);
        (void)emit_event(core, &ev);
        return -1;
    }

    next = *cfg;
    next.generation = core->cfg.generation + 1;
    if (next.generation == 0) {
        next.generation = 1; /* skip 0 after wrap */
    }
    core->cfg = next;
    core->cfg_applied = 1;

    ev.type = EDGE_EVENT_CONFIG_APPLIED;
    snprintf(ev.reason, sizeof(ev.reason), "CONFIG_APPLIED gen=%llu port=%u",
             (unsigned long long)core->cfg.generation,
             (unsigned)core->cfg.listen_port);
    (void)emit_event(core, &ev);
    return 0;
}

int edgecore_notify_config_rejected(edgecore_t *core, const char *reason)
{
    edge_event_t ev;

    if (!core) {
        return -1;
    }
    memset(&ev, 0, sizeof(ev));
    ev.type = EDGE_EVENT_CONFIG_REJECTED;
    if (reason && reason[0]) {
        snprintf(ev.reason, sizeof(ev.reason), "%s", reason);
    } else {
        snprintf(ev.reason, sizeof(ev.reason), "CONFIG_REJECTED");
    }
    return emit_event(core, &ev) == 0 ? 0 : -1;
}

const edge_config_t *edgecore_config(const edgecore_t *core)
{
    return core ? &core->cfg : NULL;
}
