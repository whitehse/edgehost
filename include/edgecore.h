/**
 * @file edgecore.h
 * @brief Syscall-free edgecore library (buffers + pull events).
 *
 * Host owns sockets, files, io_uring, TLS, signals, and process malloc for
 * edgecore-owned growth (host_alloc / NEED_ALLOC — P1.2). P1.1 skeleton:
 * create / destroy / next_event over a fixed ring.
 *
 * See docs/decisions/002-core-host-split.md and program design Track 1.
 */
#ifndef EDGECORE_H
#define EDGECORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGECORE_VERSION_MAJOR 0
#define EDGECORE_VERSION_MINOR 1
#define EDGECORE_VERSION_PATCH 0

/**
 * Core output events. Payload fields grow in later PRs; P1.1 only defines
 * the kind enum and a reason string for diagnostics.
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

typedef struct {
    edge_event_type_t type;
    char              reason[96];
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
void        edgecore_destroy(edgecore_t *core);

/**
 * Pull next event.
 * @return 1 if @p ev filled, 0 if queue empty, -1 on error (null args).
 */
int edgecore_next_event(edgecore_t *core, edge_event_t *ev);

int      edgecore_has_pending_events(const edgecore_t *core);
size_t   edgecore_event_count(const edgecore_t *core);
uint64_t edgecore_dropped_count(const edgecore_t *core);

const char *edgecore_event_type_name(edge_event_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* EDGECORE_H */
