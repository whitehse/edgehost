/**
 * @file edgecore.h
 * @brief Syscall-free edgecore library (buffers + pull events).
 *
 * Host owns sockets, files, io_uring, TLS, signals, and process malloc for
 * edgecore-owned data growth via host_alloc + NEED_ALLOC / NEED_REALLOC
 * (ADR-003 / Decision X1).
 *
 * See docs/decisions/002-core-host-split.md and 003-event-gated-memory.md.
 */
#ifndef EDGECORE_H
#define EDGECORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGECORE_VERSION_MAJOR 0
#define EDGECORE_VERSION_MINOR 2
#define EDGECORE_VERSION_PATCH 0

/** Max simultaneous host-owned data buffers registered with one core. */
#define EDGECORE_MAX_BUFS 32

/**
 * Core output events. NEED_* carry alloc fields; other kinds grow later.
 */
typedef enum {
    EDGE_EVENT_NONE = 0,
    EDGE_EVENT_NEED_ALLOC,
    EDGE_EVENT_NEED_REALLOC,
    EDGE_EVENT_WANT_SEND,
    EDGE_EVENT_HTTP_REQUEST,
    EDGE_EVENT_STATE_CHANGED,
    EDGE_EVENT_WS_BROADCAST,
    EDGE_EVENT_CONFIG_APPLIED,
    EDGE_EVENT_CONFIG_REJECTED,
    EDGE_EVENT_OUTBOUND_DONE,  /* host-internal: demux to plugin on_http_complete */
    EDGE_EVENT_QUEUE_OVERFLOW
} edge_event_type_t;

/** Why core is asking the host for memory. */
typedef enum {
    EDGE_BUF_NONE = 0,
    EDGE_BUF_GENERIC,
    EDGE_BUF_CONN_RX,
    EDGE_BUF_CONN_TX,
    EDGE_BUF_HTTP_BODY,
    EDGE_BUF_STATE
} edge_buf_kind_t;

typedef struct {
    edge_event_type_t type;
    char              reason[96];
    /* Valid for EDGE_EVENT_NEED_ALLOC / EDGE_EVENT_NEED_REALLOC */
    uint32_t          alloc_id;
    edge_buf_kind_t   buf_kind;
    size_t            size;     /* requested size */
    void             *old_ptr;  /* NEED_REALLOC: previous base (may be NULL) */
    size_t            old_size;
} edge_event_t;

typedef struct {
    /**
     * Event ring capacity. 0 = default (64). Hard max 256.
     * Allocated entirely at create (no silent grow after create).
     */
    size_t event_queue_size;
} edgecore_config_t;

typedef struct edgecore edgecore_t;

/** Defaults: event_queue_size = 0 (library default 64). */
edgecore_config_t edgecore_default_config(void);

edgecore_t *edgecore_create(void);
edgecore_t *edgecore_create_with_config(const edgecore_config_t *cfg);

/**
 * Destroy core bookkeeping. Does **not** free host-provided buffers —
 * host must detach + host_free (or free after for_each) first.
 */
void edgecore_destroy(edgecore_t *core);

/**
 * Pull next event.
 * @return 1 if @p ev filled, 0 if queue empty, -1 on error (null args).
 */
int edgecore_next_event(edgecore_t *core, edge_event_t *ev);

int      edgecore_has_pending_events(const edgecore_t *core);
size_t   edgecore_event_count(const edgecore_t *core);
uint64_t edgecore_dropped_count(const edgecore_t *core);

const char *edgecore_event_type_name(edge_event_type_t type);
const char *edgecore_buf_kind_name(edge_buf_kind_t kind);

/* --- Event-gated memory (P1.2 / ADR-003) --- */

/**
 * Request a new host-owned buffer. Enqueues EDGE_EVENT_NEED_ALLOC.
 * @return alloc_id > 0 on success, 0 if no free slot or queue overflow.
 */
uint32_t edgecore_request_alloc(edgecore_t *core, edge_buf_kind_t kind,
                                size_t size);

/**
 * Request grow of an existing ready buffer. Enqueues EDGE_EVENT_NEED_REALLOC.
 * @return 0 on success, -1 if id unknown / not ready / queue full.
 */
int edgecore_request_realloc(edgecore_t *core, uint32_t alloc_id,
                             size_t new_size);

/**
 * Host fulfills NEED_ALLOC or NEED_REALLOC.
 * @p size must be >= the requested size. Ownership of @p ptr stays with host
 * for free purposes; core only stores the pointer.
 * @return 0 ok, -1 invalid id/state/size.
 */
int edgecore_provide_buffer(edgecore_t *core, uint32_t alloc_id, void *ptr,
                            size_t size);

/**
 * Core forgets buffer pointer (does not free). Optional out params for host.
 * @return 0 ok, -1 unknown id.
 */
int edgecore_detach_buffer(edgecore_t *core, uint32_t alloc_id, void **out_ptr,
                           size_t *out_size);

const void *edgecore_buffer_ptr(const edgecore_t *core, uint32_t alloc_id);
size_t      edgecore_buffer_size(const edgecore_t *core, uint32_t alloc_id);
size_t      edgecore_buffer_count(const edgecore_t *core);

#ifdef __cplusplus
}
#endif

#endif /* EDGECORE_H */
