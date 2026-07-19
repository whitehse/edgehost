/**
 * @file edge_auth_host.h
 * @brief Host helpers: load auth secrets from env using config (P1.7c).
 */
#ifndef EDGE_AUTH_HOST_H
#define EDGE_AUTH_HOST_H

#include "edge_auth.h"
#include "edge_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Build auth context from config + environment.
 * - mode open: always succeeds; no secrets required.
 * - mode lab_password: requires non-empty password and HMAC key env vars.
 * @return 0 ok, -1 missing/invalid secrets (message in err).
 */
int edge_auth_ctx_from_config(const edge_config_t *cfg, edge_auth_ctx_t *out,
                              char *err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_AUTH_HOST_H */
