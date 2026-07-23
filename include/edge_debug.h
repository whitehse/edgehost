/**
 * @file edge_debug.h
 * @brief Lab diagnostics: hierarchical memory report + CPU flame sampling.
 *
 * Memory: process RSS + host_alloc kinds + per-module itemization (E7 shelves/
 * sessions, HTTP conns, state namespaces, WS). Served at GET /api/v1/debug/memory.
 *
 * CPU: on-demand SIGPROF + backtrace() sampler (no root required). Optionally
 * tries perf_event_open for hardware-backed sampling when the kernel allows it.
 * Folded stacks + flame tree JSON for lab console flame graph.
 * eBPF/bpftrace/perf CLI are detected as external capabilities when present.
 */
#ifndef EDGE_DEBUG_H
#define EDGE_DEBUG_H

#include "edge_e7_callhome.h"
#include "edge_metrics.h"
#include "edge_state.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Live HTTP / io_uring snapshot (published by the accept loop). */
typedef struct {
    int      valid;
    int      max_conns;
    int      active_conns;
    int      ws_conns;
    size_t   send_cap;           /* per-conn send buffer capacity */
    size_t   conn_table_bytes;   /* host_alloc: conn_t[] */
    size_t   send_bufs_live;     /* active_conns * send_cap (approx) */
    size_t   recv_bufs_embedded; /* active_conns * sizeof(recv_buf) */
    uint64_t accepts;
    uint64_t requests;
} edge_debug_http_snap_t;

/** Publish current HTTP live counters (safe to call from host loop). */
void edge_debug_http_publish(const edge_debug_http_snap_t *snap);

/** Copy last published HTTP snap (zeros if never published). */
void edge_debug_http_get(edge_debug_http_snap_t *out);

/**
 * Build hierarchical memory JSON into @p buf.
 * Includes process, host_alloc, modules[{id,name,bytes,items[]}], unaccounted.
 * @return bytes written excl NUL, or -1.
 */
int edge_debug_memory_json(const edge_metrics_t *metrics,
                           const edge_state_store_t *state,
                           const edge_e7_callhome_t *e7, char *buf,
                           size_t buf_sz);

/** Capabilities / sampler status JSON. */
int edge_debug_cpu_capabilities_json(char *buf, size_t buf_sz);

/**
 * Start a CPU profile for @p seconds (1..60). Mode: "auto" | "sigprof" | "perf".
 * @return 0 ok, -1 busy/error (message in @p err, err_sz).
 */
int edge_debug_cpu_profile_start(int seconds, const char *mode, char *err,
                                 size_t err_sz);

/** Status + optional result: running|done|idle|error. */
int edge_debug_cpu_profile_status_json(char *buf, size_t buf_sz);

/**
 * Folded stacks body (text/plain style lines "a;b;c count"). Empty if none.
 * @return bytes written excl NUL, or -1.
 */
int edge_debug_cpu_profile_folded(char *buf, size_t buf_sz);

/** Flame tree JSON {name,value,children:[...]}. */
int edge_debug_cpu_profile_flame_json(char *buf, size_t buf_sz);

/**
 * Tick from host loop: take a wall-clock sample when profiling, finish when
 * deadline reached. Prefer this over signals (backtrace from normal context).
 */
void edge_debug_cpu_on_tick(uint64_t mono_ms);

/** 1 while a profile is running (host may tighten poll timeout). */
int edge_debug_cpu_profiling(void);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_DEBUG_H */
