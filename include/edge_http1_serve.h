/**
 * @file edge_http1_serve.h
 * @brief Shared shaggy HTTP/1 request → response (health, SPA, packages).
 *
 * Used by production io_uring loop and class-A sim_main / fuzz.
 * Static file I/O is host-side (edge_static) when doc roots are set.
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

/** Optional roots for static SPA + packages (not copied; must outlive serve). */
typedef struct {
    const char *spa_root;       /* NULL → no SPA files; GET / → plain ok */
    const char *packages_root;  /* NULL → /packages/ → 404 */
    size_t      max_file_bytes; /* 0 → EDGE_STATIC_MAX_FILE */
} edge_http1_docroot_t;

edge_http1_serve_t *edge_http1_serve_create(void);
void                edge_http1_serve_destroy(edge_http1_serve_t *s);
void                edge_http1_serve_reset(edge_http1_serve_t *s);

/** Set/clear document roots (pointers not owned). */
void edge_http1_serve_set_docroots(edge_http1_serve_t *s,
                                   const edge_http1_docroot_t *roots);

/**
 * Feed inbound bytes. Updates @p metrics when a response is produced.
 * @return 1 response ready in out, 0 need more data, -1 hard error
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
