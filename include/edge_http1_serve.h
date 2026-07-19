/**
 * @file edge_http1_serve.h
 * @brief Shared shaggy HTTP/1 request → response (health, SPA, packages, state).
 */
#ifndef EDGE_HTTP1_SERVE_H
#define EDGE_HTTP1_SERVE_H

#include "edge_metrics.h"
#include "edge_state.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct edge_http1_serve edge_http1_serve_t;

typedef struct {
    const char *spa_root;
    const char *packages_root;
    size_t      max_file_bytes;
} edge_http1_docroot_t;

edge_http1_serve_t *edge_http1_serve_create(void);
void                edge_http1_serve_destroy(edge_http1_serve_t *s);
void                edge_http1_serve_reset(edge_http1_serve_t *s);

void edge_http1_serve_set_docroots(edge_http1_serve_t *s,
                                   const edge_http1_docroot_t *roots);

/** Attach state store (not owned; may be NULL). */
void edge_http1_serve_set_state(edge_http1_serve_t *s, edge_state_store_t *st);

/**
 * Feed inbound bytes.
 * @return 1 response ready, 0 need more, -1 hard error
 */
int edge_http1_serve_feed(edge_http1_serve_t *s, const uint8_t *data, size_t len,
                          edge_metrics_t *metrics, char *out, size_t out_cap,
                          size_t *out_len);

const char *edge_http1_serve_method(const edge_http1_serve_t *s);
const char *edge_http1_serve_path(const edge_http1_serve_t *s);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_HTTP1_SERVE_H */
