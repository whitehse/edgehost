/**
 * @file edge_openai_proxy.h
 * @brief OpenAI-compatible HTTP proxy plugin (P1.8b / ADR-014).
 */
#ifndef EDGE_OPENAI_PROXY_H
#define EDGE_OPENAI_PROXY_H

#include "edge_plugin.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGE_OPENAI_URL_MAX   512
#define EDGE_OPENAI_HOST_MAX  256
#define EDGE_OPENAI_ADDR_MAX  64
#define EDGE_OPENAI_ENV_MAX   64
#define EDGE_OPENAI_KEY_MAX   256

typedef struct {
    int      enabled;
    char     upstream[EDGE_OPENAI_URL_MAX];       /* e.g. https://api.openai.com */
    char     upstream_addr[EDGE_OPENAI_ADDR_MAX]; /* optional IP skip DNS */
    char     upstream_host[EDGE_OPENAI_HOST_MAX]; /* SNI/Host override */
    char     api_key_env[EDGE_OPENAI_ENV_MAX];    /* default OPENAI_API_KEY */
    char     service_api_key_env[EDGE_OPENAI_ENV_MAX]; /* optional service bearer */
    uint32_t timeout_ms;
    uint32_t rate_limit_rpm;              /* 0 = disabled */
    uint32_t max_concurrent_per_principal;
    char     path_prefix[64];             /* default /v1 */
} edge_openai_proxy_config_t;

void edge_openai_proxy_config_defaults(edge_openai_proxy_config_t *c);

/**
 * Fill plugin object (static storage OK). Call before host register.
 * Loads API key from env named by cfg. Returns 0 ok, -1 if enabled but no key.
 */
int edge_openai_proxy_init_plugin(edge_plugin_t *plugin,
                                  edge_openai_proxy_config_t *cfg_storage,
                                  const edge_openai_proxy_config_t *cfg);

const edge_plugin_vtbl_t *edge_openai_proxy_vtbl(void);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_OPENAI_PROXY_H */
