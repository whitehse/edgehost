/**
 * @file host_alloc.c
 * @brief Tagged process malloc gate with accurate byte accounting.
 */

#include "host_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_ALLOC_MAGIC 0x45484d41u /* 'EHMA' */

typedef struct {
    size_t   size;  /* user payload bytes */
    uint32_t kind;
    uint32_t magic;
} host_alloc_hdr_t;

typedef struct {
    uint64_t allocs;
    uint64_t frees;
    uint64_t bytes;
    uint64_t peak;
    uint64_t note_bytes; /* outstanding from host_mem_note only */
    uint64_t note_peak;
} host_kind_stats_t;

static host_kind_stats_t g_kind[EDGE_MEM_KIND_COUNT];
static uint64_t          g_allocs;
static uint64_t          g_reallocs;
static uint64_t          g_frees;
static uint64_t          g_bytes;
static uint64_t          g_peak;

static int kind_ok(edge_mem_kind_t kind)
{
    return (unsigned)kind < (unsigned)EDGE_MEM_KIND_COUNT;
}

static host_alloc_hdr_t *hdr_from_user(void *p)
{
    if (!p) {
        return NULL;
    }
    return (host_alloc_hdr_t *)((char *)p - sizeof(host_alloc_hdr_t));
}

static void *user_from_hdr(host_alloc_hdr_t *h)
{
    return (void *)(h + 1);
}

static void note_alloc(edge_mem_kind_t kind, size_t n)
{
    host_kind_stats_t *ks;

    if (!kind_ok(kind)) {
        kind = EDGE_MEM_OTHER;
    }
    ks = &g_kind[kind];
    ks->allocs++;
    ks->bytes += (uint64_t)n;
    if (ks->bytes > ks->peak) {
        ks->peak = ks->bytes;
    }
    g_allocs++;
    g_bytes += (uint64_t)n;
    if (g_bytes > g_peak) {
        g_peak = g_bytes;
    }
}

static void note_free(edge_mem_kind_t kind, size_t n)
{
    host_kind_stats_t *ks;

    if (!kind_ok(kind)) {
        kind = EDGE_MEM_OTHER;
    }
    ks = &g_kind[kind];
    ks->frees++;
    if (ks->bytes >= (uint64_t)n) {
        ks->bytes -= (uint64_t)n;
    } else {
        ks->bytes = 0;
    }
    g_frees++;
    if (g_bytes >= (uint64_t)n) {
        g_bytes -= (uint64_t)n;
    } else {
        g_bytes = 0;
    }
}

static void note_resize(edge_mem_kind_t kind, size_t old_n, size_t new_n)
{
    host_kind_stats_t *ks;

    if (!kind_ok(kind)) {
        kind = EDGE_MEM_OTHER;
    }
    ks = &g_kind[kind];
    if (ks->bytes >= (uint64_t)old_n) {
        ks->bytes -= (uint64_t)old_n;
    } else {
        ks->bytes = 0;
    }
    ks->bytes += (uint64_t)new_n;
    if (ks->bytes > ks->peak) {
        ks->peak = ks->bytes;
    }
    if (g_bytes >= (uint64_t)old_n) {
        g_bytes -= (uint64_t)old_n;
    } else {
        g_bytes = 0;
    }
    g_bytes += (uint64_t)new_n;
    if (g_bytes > g_peak) {
        g_peak = g_bytes;
    }
}

void host_alloc_reset_stats(void)
{
    memset(g_kind, 0, sizeof(g_kind));
    g_allocs = 0;
    g_reallocs = 0;
    g_frees = 0;
    g_bytes = 0;
    g_peak = 0;
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

uint64_t host_bytes_peak(void)
{
    return g_peak;
}

uint64_t host_alloc_count_kind(edge_mem_kind_t kind)
{
    if (!kind_ok(kind)) {
        return 0;
    }
    return g_kind[kind].allocs;
}

uint64_t host_free_count_kind(edge_mem_kind_t kind)
{
    if (!kind_ok(kind)) {
        return 0;
    }
    return g_kind[kind].frees;
}

uint64_t host_bytes_outstanding_kind(edge_mem_kind_t kind)
{
    if (!kind_ok(kind)) {
        return 0;
    }
    return g_kind[kind].bytes + g_kind[kind].note_bytes;
}

uint64_t host_bytes_peak_kind(edge_mem_kind_t kind)
{
    uint64_t p;

    if (!kind_ok(kind)) {
        return 0;
    }
    p = g_kind[kind].peak;
    if (g_kind[kind].note_peak > p) {
        p = g_kind[kind].note_peak;
    }
    return p;
}

const char *edge_mem_kind_name(edge_mem_kind_t kind)
{
    switch (kind) {
    case EDGE_MEM_EDGECORE:
        return "edgecore";
    case EDGE_MEM_HTTP:
        return "http";
    case EDGE_MEM_E7:
        return "e7";
    case EDGE_MEM_WS:
        return "ws";
    case EDGE_MEM_STATE:
        return "state";
    case EDGE_MEM_PLUGIN:
        return "plugin";
    case EDGE_MEM_OTHER:
        return "other";
    default:
        return "unknown";
    }
}

void host_mem_note(edge_mem_kind_t kind, int64_t delta_bytes)
{
    host_kind_stats_t *ks;

    if (!kind_ok(kind) || delta_bytes == 0) {
        return;
    }
    ks = &g_kind[kind];
    if (delta_bytes > 0) {
        ks->note_bytes += (uint64_t)delta_bytes;
        if (ks->note_bytes > ks->note_peak) {
            ks->note_peak = ks->note_bytes;
        }
    } else {
        uint64_t sub = (uint64_t)(-delta_bytes);
        if (ks->note_bytes >= sub) {
            ks->note_bytes -= sub;
        } else {
            ks->note_bytes = 0;
        }
    }
}

void *host_alloc(size_t n)
{
    return host_alloc_kind(EDGE_MEM_EDGECORE, n);
}

void *host_alloc_kind(edge_mem_kind_t kind, size_t n)
{
    host_alloc_hdr_t *h;

    if (n == 0) {
        return NULL;
    }
    if (!kind_ok(kind)) {
        kind = EDGE_MEM_OTHER;
    }
    h = (host_alloc_hdr_t *)calloc(1, sizeof(*h) + n);
    if (!h) {
        return NULL;
    }
    h->size = n;
    h->kind = (uint32_t)kind;
    h->magic = HOST_ALLOC_MAGIC;
    note_alloc(kind, n);
    return user_from_hdr(h);
}

void *host_realloc(void *p, size_t n)
{
    return host_realloc_kind(EDGE_MEM_EDGECORE, p, n);
}

void *host_realloc_kind(edge_mem_kind_t kind, void *p, size_t n)
{
    host_alloc_hdr_t *h;
    host_alloc_hdr_t *q;
    size_t            old_n;
    edge_mem_kind_t   old_kind;

    if (n == 0) {
        host_free(p);
        return NULL;
    }
    if (!p) {
        return host_alloc_kind(kind, n);
    }
    h = hdr_from_user(p);
    if (h->magic != HOST_ALLOC_MAGIC) {
        /* Not a host_alloc block — refuse to corrupt accounting. */
        return NULL;
    }
    old_n = h->size;
    old_kind = (edge_mem_kind_t)h->kind;
    q = (host_alloc_hdr_t *)realloc(h, sizeof(*q) + n);
    if (!q) {
        return NULL;
    }
    q->size = n;
    q->kind = (uint32_t)old_kind;
    q->magic = HOST_ALLOC_MAGIC;
    note_resize(old_kind, old_n, n);
    g_reallocs++;
    return user_from_hdr(q);
}

void host_free(void *p)
{
    host_alloc_hdr_t *h;

    if (!p) {
        return;
    }
    h = hdr_from_user(p);
    if (h->magic != HOST_ALLOC_MAGIC) {
        /* Refuse to free unknown pointers through this gate. */
        return;
    }
    note_free((edge_mem_kind_t)h->kind, h->size);
    h->magic = 0;
    free(h);
}

int host_mem_process_kb(uint64_t *vm_rss_kb, uint64_t *vm_hwm_kb,
                        uint64_t *vm_size_kb, uint64_t *vm_peak_kb)
{
    FILE *f;
    char  line[256];
    int   found = 0;

    if (vm_rss_kb) {
        *vm_rss_kb = 0;
    }
    if (vm_hwm_kb) {
        *vm_hwm_kb = 0;
    }
    if (vm_size_kb) {
        *vm_size_kb = 0;
    }
    if (vm_peak_kb) {
        *vm_peak_kb = 0;
    }

    f = fopen("/proc/self/status", "r");
    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        unsigned long v = 0;
        if (vm_rss_kb && sscanf(line, "VmRSS: %lu", &v) == 1) {
            *vm_rss_kb = (uint64_t)v;
            found = 1;
        } else if (vm_hwm_kb && sscanf(line, "VmHWM: %lu", &v) == 1) {
            *vm_hwm_kb = (uint64_t)v;
            found = 1;
        } else if (vm_size_kb && sscanf(line, "VmSize: %lu", &v) == 1) {
            *vm_size_kb = (uint64_t)v;
            found = 1;
        } else if (vm_peak_kb && sscanf(line, "VmPeak: %lu", &v) == 1) {
            *vm_peak_kb = (uint64_t)v;
            found = 1;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

int host_mem_format_json(char *buf, size_t buflen)
{
    uint64_t rss = 0, hwm = 0, size = 0, peak = 0;
    char     kinds[768];
    size_t   koff = 0;
    int      i;
    int      n;

    if (!buf || buflen < 64) {
        return -1;
    }
    (void)host_mem_process_kb(&rss, &hwm, &size, &peak);

    kinds[0] = '\0';
    for (i = 0; i < (int)EDGE_MEM_KIND_COUNT; i++) {
        int kn;
        uint64_t out_b = host_bytes_outstanding_kind((edge_mem_kind_t)i);
        uint64_t peak_b = host_bytes_peak_kind((edge_mem_kind_t)i);
        uint64_t al = host_alloc_count_kind((edge_mem_kind_t)i);
        uint64_t fr = host_free_count_kind((edge_mem_kind_t)i);
        uint64_t note = g_kind[i].note_bytes;

        kn = snprintf(kinds + koff, sizeof(kinds) - koff,
                      "%s\"%s\":{"
                      "\"bytes\":%llu,"
                      "\"peak_bytes\":%llu,"
                      "\"allocs\":%llu,"
                      "\"frees\":%llu,"
                      "\"note_bytes\":%llu"
                      "}",
                      koff ? "," : "", edge_mem_kind_name((edge_mem_kind_t)i),
                      (unsigned long long)out_b, (unsigned long long)peak_b,
                      (unsigned long long)al, (unsigned long long)fr,
                      (unsigned long long)note);
        if (kn < 0 || (size_t)kn >= sizeof(kinds) - koff) {
            return -1;
        }
        koff += (size_t)kn;
    }

    n = snprintf(buf, buflen,
                 "{"
                 "\"process\":{"
                 "\"vm_rss_kb\":%llu,"
                 "\"vm_hwm_kb\":%llu,"
                 "\"vm_size_kb\":%llu,"
                 "\"vm_peak_kb\":%llu"
                 "},"
                 "\"host_alloc\":{"
                 "\"bytes\":%llu,"
                 "\"peak_bytes\":%llu,"
                 "\"allocs\":%llu,"
                 "\"reallocs\":%llu,"
                 "\"frees\":%llu,"
                 "\"by_kind\":{%s}"
                 "}"
                 "}",
                 (unsigned long long)rss, (unsigned long long)hwm,
                 (unsigned long long)size, (unsigned long long)peak,
                 (unsigned long long)g_bytes, (unsigned long long)g_peak,
                 (unsigned long long)g_allocs, (unsigned long long)g_reallocs,
                 (unsigned long long)g_frees, kinds);
    if (n < 0 || (size_t)n >= buflen) {
        return -1;
    }
    return n;
}
