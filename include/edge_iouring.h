/**
 * @file edge_iouring.h
 * @brief Class-A io_uring accept loop with shaggy HTTP/1 parse (P1.4b).
 *
 * Plain TCP: accept → recv → http1_feed_input → static simple response.
 * TLS (OpenSSL non-blocking) lands in P1.13. Real /health JSON is P1.4c.
 */
#ifndef EDGE_IOURING_H
#define EDGE_IOURING_H

#include "edge_config.h"

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
     * Optional override for the successful (parsed) response body payload.
     * If NULL, built-in "ok\n" is used. Status line / headers still built
     * by the host after shaggy HEADERS_COMPLETE.
     * Not copied — must remain valid for the duration of edge_iouring_run.
     */
    const char *static_body;
    size_t      static_body_len;
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
