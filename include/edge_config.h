/**
 * @file edge_config.h
 * @brief edgehost runtime configuration (YAML-backed; P1.3 / ADR-005).
 *
 * Fixed-size fields — no heap in the config struct. Host loads via
 * edge_config_load_yaml_*; core applies via edgecore_apply_config.
 */
#ifndef EDGE_CONFIG_H
#define EDGE_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGE_CONFIG_HOST_MAX 256
#define EDGE_CONFIG_PATH_MAX 512

typedef struct {
    char     listen_host[EDGE_CONFIG_HOST_MAX];
    uint16_t listen_port;

    /** Document root for company SPA (index.html, assets). */
    char spa_root[EDGE_CONFIG_PATH_MAX];
    /**
     * Root for map/package files (libwebmap .wmap etc.), served under
     * URL prefix /packages/ (P1.6).
     */
    char packages_root[EDGE_CONFIG_PATH_MAX];
    /** Max static file bytes loaded into response buffer (0 → default 64 KiB). */
    size_t static_max_file_bytes;

    size_t http_max_body_bytes;
    size_t http_max_pending_outbound;
    size_t http_max_upstream_body_bytes;

    /** v1 namespaces (1 = enabled). */
    int state_net_core_enabled;
    int state_map_dynamic_enabled;

    /**
     * Auth (P1.7c / ADR-013). mode: open | lab_password | proxy_headers.
     * Secrets are loaded from env named by *_env fields (host-side).
     */
    char     auth_mode[32];                 /* default "open" */
    char     auth_lab_password_env[64];     /* default EDGEHOST_LAB_PASSWORD */
    char     auth_session_hmac_key_env[64]; /* default EDGEHOST_SESSION_HMAC */
    char     auth_proxy_hmac_key_env[64];   /* default EDGEHOST_PROXY_HMAC */
    uint32_t auth_session_ttl_s;            /* default 28800 */
    uint32_t auth_proxy_max_skew_s;         /* default 300 */

    /** DNS policy (P1.8b outbound). */
    int dns_allow_blocking; /* lab only; default 0 */

    /**
     * openai_proxy plugin (P1.8b). Secrets from env; never stored from YAML.
     */
    int      openai_enabled;
    char     openai_upstream[512];
    char     openai_upstream_addr[64];
    char     openai_upstream_host[256];
    char     openai_api_key_env[64];
    char     openai_service_key_env[64];
    uint32_t openai_timeout_ms;
    uint32_t openai_rate_limit_rpm;
    uint32_t openai_max_concurrent;

    /**
     * Set by edgecore_apply_config on success (not loaded from YAML).
     * Monotonic generation for tests and observability.
     */
    uint64_t generation;
} edge_config_t;

/** Fill @p c with safe lab defaults. */
void edge_config_defaults(edge_config_t *c);

/**
 * Validate structural constraints (port, sizes, host non-empty).
 * @return 0 ok, -1 invalid (message in @p err if provided).
 */
int edge_config_validate(const edge_config_t *c, char *err, size_t err_len);

/**
 * Load YAML from memory into @p out (starts from defaults, then overlays).
 * Uses sibling libyaml. Host-side I/O only via this and path variant.
 * @return 0 ok, -1 on parse/validate-prep failure.
 */
int edge_config_load_yaml_buf(const char *yaml, size_t yaml_len,
                              edge_config_t *out, char *err, size_t err_len);

/**
 * Read file then load_yaml_buf. Empty file → defaults.
 * @return 0 ok, -1 on I/O or parse failure.
 */
int edge_config_load_yaml_path(const char *path, edge_config_t *out,
                               char *err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_CONFIG_H */
