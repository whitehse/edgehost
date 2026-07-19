/**
 * @file edge_iouring.h
 * @brief Class-A io_uring accept loop + shaggy HTTP/1 (P1.4c).
 *
 * Routes:
 *   GET /health  → application/json metrics (edge_metrics)
 *   other GET    → text/plain "ok\n" (compat)
 *   parse error  → 400
 *
 * TLS (OpenSSL non-blocking) lands in P1.13.
 */
#ifndef EDGE_IOURING_H
#define EDGE_IOURING_H

#include "edge_auth.h"
#include "edge_config.h"
#include "edge_e7_callhome.h"
#include "edge_metrics.h"
#include "edge_plugin_host.h"
#include "edge_state.h"

#include <signal.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int ring_entries;   /* 0 = default 256 */
    int max_conns;      /* 0 = default 64 */
    int backlog;        /* 0 = default 128 */
    /**
     * Stop after this many successful accepts (0 = unlimited).
     * Useful for integration tests.
     */
    int max_accepts;
    /** Optional external stop flag (SIGINT handler may set it). */
    volatile sig_atomic_t *stop;
    /**
     * Optional override for non-/health successful body (default "ok\n").
     * Not copied — must remain valid for the duration of edge_iouring_run.
     */
    const char *static_body;
    size_t      static_body_len;
    /**
     * Optional metrics sink. If NULL, an internal counter set is used for
     * /health only. If non-NULL, the same counters are updated in place
     * (caller may read after run returns, e.g. max_accepts tests).
     */
    edge_metrics_t *metrics;
    /**
     * Optional state store (P1.7a). If NULL, a store is created for the run
     * and destroyed on exit. Not destroyed if caller-provided.
     */
    edge_state_store_t *state;
    /**
     * Optional auth context (P1.7c). If NULL, auth mode OPEN (no checks).
     * Not destroyed (caller-owned).
     */
    edge_auth_ctx_t *auth;
    /**
     * Optional plugin host (P1.8b openai_proxy). Not destroyed.
     */
    edge_plugin_host_t *plugins;
    /** Service API key for Bearer → service_openai (optional). */
    const char *service_api_key;
    /**
     * Optional E7 Call Home engine (PR-4a). Not destroyed (caller-owned).
     * When non-NULL and enabled, second listen fd is accepted via domain demux
     * (UD_DOMAIN_E7); session I/O is pumped on tick (poll path).
     */
    edge_e7_callhome_t *e7;
} edge_iouring_opts_t;

void edge_iouring_opts_defaults(edge_iouring_opts_t *o);

/**
 * Bind/listen on cfg->listen_host:listen_port, run io_uring accept/recv/send
 * until stop or max_accepts (if set) and all connections drained.
 *
 * @return 0 clean exit, 1 bind/uring failure, 2 interrupted with error.
 */
int edge_iouring_run(const edge_config_t *cfg, const edge_iouring_opts_t *opts);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_IOURING_H */
