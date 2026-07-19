/**
 * @file edge_http1_serve.h
 * @brief Shared shaggy HTTP/1 request → host-built response (P1.4b/c, P1.5).
 *
 * Used by production io_uring loop and class-A sim_main / fuzz (no sockets).
 */
#ifndef EDGE_HTTP1_SERVE_H
#define EDGE_HTTP1_SERVE_H

#include "edge_metrics.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct edge_http1_serve edge_http1_serve_t;

edge_http1_serve_t *edge_http1_serve_create(void);
void                edge_http1_serve_destroy(edge_http1_serve_t *s);
void                edge_http1_serve_reset(edge_http1_serve_t *s);

/**
 * Feed inbound bytes. Updates @p metrics (bytes_in, and request counters when
 * a response is produced).
 *
 * @param out      filled when a complete response is ready
 * @param out_cap  capacity of @p out
 * @param out_len  set to response length when return is 1
 * @return 1 response ready in out, 0 need more data, -1 hard error
 */
int edge_http1_serve_feed(edge_http1_serve_t *s, const uint8_t *data, size_t len,
                          edge_metrics_t *metrics, char *out, size_t out_cap,
                          size_t *out_len);

/** Last parsed method/path (valid after headers complete). */
const char *edge_http1_serve_method(const edge_http1_serve_t *s);
const char *edge_http1_serve_path(const edge_http1_serve_t *s);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_HTTP1_SERVE_H */
