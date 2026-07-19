/**
 * @file edge_outbound.h
 * @brief Host outbound HTTP(S) client for plugin PENDING (P1.8b).
 *
 * Phase-1: blocking connect/read on the calling thread (uring or test).
 * Prefer upstream_addr to skip DNS; blocking getaddrinfo only when
 * dns.allow_blocking is true. HTTPS via OpenSSL. True non-blocking TLS
 * client on the ring is refined in P1.13b.
 */
#ifndef EDGE_OUTBOUND_H
#define EDGE_OUTBOUND_H

#include "edge_plugin.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      allow_blocking_dns; /* lab only */
    size_t   max_response_body;  /* 0 → 4 MiB */
    uint32_t default_timeout_ms; /* 0 → 60000 */
} edge_outbound_opts_t;

void edge_outbound_opts_defaults(edge_outbound_opts_t *o);

/**
 * Execute one HTTP/1.1 request (http or https).
 * Fills out->status/body when transport_err==0.
 * @p out->body must be pre-allocated (body_cap).
 * @return 0 if a result was produced (check transport_err), -1 hard error.
 */
int edge_outbound_http_execute(const edge_http_client_req_t *req,
                               const edge_outbound_opts_t *opts,
                               edge_http_client_result_t *out, uint8_t *body_buf,
                               size_t body_cap, size_t *body_len);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_OUTBOUND_H */
