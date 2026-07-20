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

/** Bootstrap allowlist capacity for plugins.e7_callhome.shelves[] (K15/K17). */
#define EDGE_CONFIG_E7_SHELVES_MAX 160
/** MAC string (e.g. 00:02:5d:d9:21:47) + NUL. */
#define EDGE_CONFIG_E7_MAC_MAX 32
/** Optional human shelf_id / label. */
#define EDGE_CONFIG_E7_SHELF_ID_MAX 64
/** transport / reload_policy short enums as strings. */
#define EDGE_CONFIG_E7_ENUM_MAX 16
/** Lab SSH Call Home password (plugins.e7_callhome.ssh_password). */
#define EDGE_CONFIG_E7_SSH_PASSWORD_MAX 128
/** Optional expected SSH username (plugins.e7_callhome.ssh_username). */
#define EDGE_CONFIG_E7_SSH_USERNAME_MAX 64
/** create-subscription <stream> name (Calix: exa-events). */
#define EDGE_CONFIG_E7_STREAM_MAX 64

/**
 * One YAML allowlist seed entry (MAC primary key — K17).
 * shelf_id is optional human label; enabled defaults true when mac set.
 */
typedef struct {
    char mac[EDGE_CONFIG_E7_MAC_MAX];
    char shelf_id[EDGE_CONFIG_E7_SHELF_ID_MAX];
    int  enabled;
} edge_e7_shelf_config_t;

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

    /**
     * State store limits + namespaces (P1.14 / ADR-007 / K10 PR-2a).
     * Underscore YAML keys map to dotted ns names (libyaml knowledge paths
     * split on '.'). Defaults: net.core + map.dynamic on; net.pon / net.home /
     * electric / inventory off until a producer enables them.
     *
     * max_keys_default / max_value_bytes feed edge_state_create_with_config.
     * Per-ns max_keys (0 = use max_keys_default) applied at enable via
     * edge_state_apply_config — tables allocate only when ns is enabled.
     */
    size_t state_max_keys_default;  /* 0 → EDGE_STATE_KEYS_DEFAULT (1024) */
    size_t state_max_value_bytes;   /* 0 → EDGE_STATE_VALUE_DEFAULT (4096) */
    int    state_net_core_enabled;
    int    state_map_dynamic_enabled;
    int    state_net_pon_enabled;
    int    state_net_home_enabled;
    int    state_electric_enabled;
    int    state_inventory_enabled;
    size_t state_net_core_max_keys;    /* 0 → store default */
    size_t state_map_dynamic_max_keys;
    size_t state_net_pon_max_keys;
    size_t state_net_home_max_keys;
    size_t state_electric_max_keys;
    size_t state_inventory_max_keys;

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

    /** Slack / Teams stubs (P1.9 / P1.10). Default disabled. */
    int slack_enabled;
    int teams_enabled;

    /**
     * TLS server (P1.13). Empty cert/key ⇒ plain TCP (lab default).
     * Paths to PEM files; not secrets themselves.
     */
    char tls_cert[EDGE_CONFIG_PATH_MAX];
    char tls_key[EDGE_CONFIG_PATH_MAX];
    char tls_client_ca[EDGE_CONFIG_PATH_MAX]; /* optional mTLS */

    /** pqproxy side-car metrics scrape (P1.11). */
    int      pqproxy_enabled;
    char     pqproxy_metrics_url[256];
    uint32_t pqproxy_scrape_interval_ms;

    /** Postgres NOTIFY apply (P1.12). Channels listed for future LISTEN. */
    int  postgres_notify_enabled;
    char postgres_listen_channel[64]; /* default map_overlay */

    /**
     * E7 NETCONF Call Home (PR-2 skeleton — config only; no listen yet).
     * YAML path: plugins.e7_callhome.*
     * If transport=raw and listen_host is not loopback, lab_insecure_raw
     * must be true (validate fails otherwise).
     */
    int      e7_enabled; /* default 0 */
    char     e7_listen_host[EDGE_CONFIG_HOST_MAX]; /* default 127.0.0.1 */
    uint16_t e7_listen_port;                       /* default 4334 */
    char     e7_transport[EDGE_CONFIG_E7_ENUM_MAX]; /* raw | ssh */
    int      e7_lab_insecure_raw; /* required true for non-loopback raw */
    char     e7_reload_policy[EDGE_CONFIG_E7_ENUM_MAX]; /* merge | replace_all */
    int      e7_auto_subscribe_unknown; /* default 0 */
    /**
     * Notification stream for create-subscription after SESSION_OPEN.
     * Calix E7 uses "exa-events" (default). Lab peers may use "NETCONF".
     */
    char     e7_subscription_stream[EDGE_CONFIG_E7_STREAM_MAX];
    uint32_t e7_dirty_cap;              /* default 8192 */
    size_t   e7_rss_budget_bytes;       /* default 256 MiB */
    uint32_t e7_max_sessions;           /* default 160 */
    /**
     * SSH Call Home lab auth (transport: ssh; requires EDGEHOST_E7_SSH_AVAILABLE).
     * Identity preamble still runs raw TCP before SSH (Calix order).
     * ssh_password: lab password for NETCONF_SSH_CALLHOME server auth.
     * ssh_username: optional expected user (empty = any).
     * host_key_path: optional host key path (empty = ephemeral ed25519).
     * ssh_allow_none_auth: lab-only USERAUTH none (default 0).
     */
    char     e7_ssh_password[EDGE_CONFIG_E7_SSH_PASSWORD_MAX];
    char     e7_ssh_username[EDGE_CONFIG_E7_SSH_USERNAME_MAX];
    char     e7_host_key_path[EDGE_CONFIG_PATH_MAX];
    int      e7_ssh_allow_none_auth; /* default 0 */
    /**
     * Optional durable runtime allowlist file (PR-10 interim; not Postgres).
     * When non-empty: loaded after YAML seed on create; rewritten on REST
     * upsert/delete and after SIGHUP apply_config. Empty = memory only.
     */
    char     e7_allowlist_path[EDGE_CONFIG_PATH_MAX];
    edge_e7_shelf_config_t e7_shelves[EDGE_CONFIG_E7_SHELVES_MAX];
    uint32_t e7_shelf_count; /* number of populated shelves[] entries */

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
