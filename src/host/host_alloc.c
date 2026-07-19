/**
 * @file host_alloc.c
 * @brief Process malloc gate for edgecore data buffers (ADR-003).
 */

#include "host_alloc.h"

#include <stdlib.h>
#include <string.h>

static uint64_t g_allocs;
static uint64_t g_reallocs;
static uint64_t g_frees;
static uint64_t g_bytes;

void host_alloc_reset_stats(void)
{
    g_allocs = 0;
    g_reallocs = 0;
    g_frees = 0;
    g_bytes = 0;
}

uint64_t host_alloc_count(void)
{
    return g_allocs;
}

uint64_t host_realloc_count(void)
{
    return g_reallocs;
}

uint64_t host_free_count(void)
{
    return g_frees;
}

uint64_t host_bytes_outstanding(void)
{
    return g_bytes;
}

void *host_alloc(size_t n)
{
    void *p;

    if (n == 0) {
        return NULL;
    }
    p = calloc(1, n);
    if (!p) {
        return NULL;
    }
    g_allocs++;
    g_bytes += (uint64_t)n;
    return p;
}

void *host_realloc(void *p, size_t n)
{
    void *q;

    if (n == 0) {
        host_free(p);
        return NULL;
    }
    if (!p) {
        return host_alloc(n);
    }
    /*
     * We do not know the previous size without a side table; stats treat
     * realloc as: free-old accounting is approximate (count only).
     * For precise byte tracking, prefer host_alloc + host_free of a new slab.
     */
    q = realloc(p, n);
    if (!q) {
        return NULL;
    }
    g_reallocs++;
    return q;
}

void host_free(void *p)
{
    if (!p) {
        return;
    }
    free(p);
    g_frees++;
}
