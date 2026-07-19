/**
 * @file host_alloc.h
 * @brief Sole process malloc gate for *edgecore-owned* data buffers (P1.2 / ADR-003).
 *
 * edgecore never calls malloc after create for growable data. Host fulfills
 * NEED_ALLOC / NEED_REALLOC by allocating here, then edgecore_provide_buffer.
 *
 * Sibling libraries (shaggy, libwebmap, …) keep their own create-time policies;
 * this gate does not wrap them.
 */
#ifndef HOST_ALLOC_H
#define HOST_ALLOC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Allocate @p n bytes (zero-filled). NULL if n==0 or OOM. */
void *host_alloc(size_t n);

/** Grow/shrink like realloc. NULL if n==0 (and frees @p). */
void *host_realloc(void *p, size_t n);

void host_free(void *p);

/* --- stats (single-threaded host / tests; not atomic) --- */
void     host_alloc_reset_stats(void);
uint64_t host_alloc_count(void);
uint64_t host_realloc_count(void);
uint64_t host_free_count(void);
uint64_t host_bytes_outstanding(void);

#ifdef __cplusplus
}
#endif

#endif /* HOST_ALLOC_H */
