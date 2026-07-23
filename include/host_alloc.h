/**
 * @file host_alloc.h
 * @brief Tagged process malloc gate for host-owned heap (P1.2 / ADR-003).
 *
 * edgecore data buffers: after create, no silent malloc — NEED_ALLOC /
 * NEED_REALLOC → host_alloc / host_realloc → edgecore_provide_buffer.
 *
 * Host subsystems (HTTP, E7, WS, …) may also allocate through this gate with
 * an edge_mem_kind_t so /health can attribute outstanding bytes by category.
 * Sibling libraries (shaggy, libwebmap, OpenSSL, …) keep their own policies;
 * their heap is only visible via process VmRSS, not host_alloc totals.
 *
 * Each live block carries a size+kind header so free/realloc update
 * outstanding bytes accurately (including after realloc grow/shrink).
 */
#ifndef HOST_ALLOC_H
#define HOST_ALLOC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Allocation category for lab /health memory breakdown. */
typedef enum {
    EDGE_MEM_EDGECORE = 0, /* edgecore NEED_ALLOC / NEED_REALLOC buffers */
    EDGE_MEM_HTTP,         /* io_uring conns, HTTP temps owned by host */
    EDGE_MEM_E7,           /* E7 Call Home sessions / tables */
    EDGE_MEM_WS,           /* WebSocket hub queues */
    EDGE_MEM_STATE,        /* reserved if state ever routes through host */
    EDGE_MEM_PLUGIN,       /* plugin host / outbound glue */
    EDGE_MEM_OTHER,        /* misc host heap */
    EDGE_MEM_KIND_COUNT
} edge_mem_kind_t;

/** Allocate @p n bytes (zero-filled), tagged EDGE_MEM_EDGECORE. NULL if n==0 or OOM. */
void *host_alloc(size_t n);

/** Allocate @p n bytes (zero-filled) under @p kind. */
void *host_alloc_kind(edge_mem_kind_t kind, size_t n);

/**
 * Grow/shrink like realloc. NULL if n==0 (and frees @p).
 * Kind is taken from the existing block header when @p is non-NULL.
 */
void *host_realloc(void *p, size_t n);

/** Realloc preserving/assigning kind (new allocs when p==NULL use @p kind). */
void *host_realloc_kind(edge_mem_kind_t kind, void *p, size_t n);

void host_free(void *p);

/* --- stats (single-threaded host / tests; not atomic) --- */
void     host_alloc_reset_stats(void);
uint64_t host_alloc_count(void);
uint64_t host_realloc_count(void);
uint64_t host_free_count(void);
uint64_t host_bytes_outstanding(void);
uint64_t host_bytes_peak(void);

uint64_t host_alloc_count_kind(edge_mem_kind_t kind);
uint64_t host_free_count_kind(edge_mem_kind_t kind);
uint64_t host_bytes_outstanding_kind(edge_mem_kind_t kind);
uint64_t host_bytes_peak_kind(edge_mem_kind_t kind);

/** Stable lowercase name for @p kind (for JSON / UI). */
const char *edge_mem_kind_name(edge_mem_kind_t kind);

/**
 * Note external (non-host_alloc) host-owned heap for breakdown purposes.
 * Use sparingly — prefer host_alloc_kind when the pointer is host-owned.
 * @p delta_bytes positive on allocate, negative on free.
 */
void host_mem_note(edge_mem_kind_t kind, int64_t delta_bytes);

/**
 * Read process memory from /proc/self/status (Linux).
 * Outputs are kilobytes as reported by the kernel. Zeroed if unavailable.
 * @return 0 on success (at least one field filled), -1 if unreadable.
 */
int host_mem_process_kb(uint64_t *vm_rss_kb, uint64_t *vm_hwm_kb,
                        uint64_t *vm_size_kb, uint64_t *vm_peak_kb);

/**
 * Format host_alloc + process memory as a JSON object (no outer braces for
 * embedding — full object including braces).
 * @return bytes written (excluding NUL), or -1 if truncated/error.
 */
int host_mem_format_json(char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* HOST_ALLOC_H */
