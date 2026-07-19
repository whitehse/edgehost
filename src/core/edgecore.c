/**
 * @file edgecore.c
 * @brief edgecore skeleton (P1.1): fixed event ring, create/destroy/next_event.
 *
 * No syscalls. Create-time allocation only (calloc/free). Post-create growth
 * will emit NEED_ALLOC / NEED_REALLOC (P1.2); this file must not call malloc
 * on the steady path.
 */

#include "edgecore.h"

#include <stdlib.h>
#include <string.h>

#define EDGECORE_DEFAULT_Q 64u
#define EDGECORE_MAX_Q     256u

struct edgecore {
    size_t         qsz;
    edge_event_t  *events;
    size_t         head;
    size_t         tail;
    size_t         cnt;
    uint64_t       dropped;
};

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
    return core;
}

void edgecore_destroy(edgecore_t *core)
{
    if (!core) {
        return;
    }
    free(core->events);
    free(core);
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
