/**
 * @file config_yaml.c
 * @brief Load edge_config_t from YAML via sibling libyaml (pqproxy pattern).
 */

#include "edge_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yaml.h"

static int parse_bool(const char *s, int *out)
{
    if (!s || !out) {
        return -1;
    }
    if (strcmp(s, "1") == 0 || strcmp(s, "true") == 0 ||
        strcmp(s, "True") == 0 || strcmp(s, "yes") == 0 ||
        strcmp(s, "on") == 0) {
        *out = 1;
        return 0;
    }
    if (strcmp(s, "0") == 0 || strcmp(s, "false") == 0 ||
        strcmp(s, "False") == 0 || strcmp(s, "no") == 0 ||
        strcmp(s, "off") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int parse_u16(const char *s, uint16_t *out)
{
    long v;
    char *end = NULL;

    if (!s || !out) {
        return -1;
    }
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0 || v > 65535) {
        return -1;
    }
    *out = (uint16_t)v;
    return 0;
}

static int parse_size(const char *s, size_t *out)
{
    unsigned long long v;
    char *end = NULL;

    if (!s || !out) {
        return -1;
    }
    v = strtoull(s, &end, 10);
    if (end == s || *end != '\0') {
        return -1;
    }
    *out = (size_t)v;
    return 0;
}

static int copy_str(char *dst, size_t dst_sz, const char *src)
{
    size_t n;

    if (!dst || dst_sz == 0 || !src) {
        return -1;
    }
    n = strlen(src);
    if (n + 1 > dst_sz) {
        return -1;
    }
    memcpy(dst, src, n + 1);
    return 0;
}

static const char *lookup(yaml_ctx_t *ctx, const char *path)
{
    size_t len = 0;
    const char *v = yaml_lookup_scalar(ctx, path, &len);

    if (!v || len == 0) {
        return NULL;
    }
    return v;
}

static int apply_scalar(edge_config_t *c, const char *key, const char *val,
                        char *err, size_t err_len)
{
    int iv;
    uint16_t u16;
    size_t sz;

    if (!key || !val) {
        return 0;
    }

#define FAIL(msg)                                                              \
    do {                                                                       \
        if (err && err_len) {                                                  \
            snprintf(err, err_len, "%s: %s", key, msg);                        \
        }                                                                      \
        return -1;                                                             \
    } while (0)

    if (strcmp(key, "listen.host") == 0) {
        if (copy_str(c->listen_host, sizeof(c->listen_host), val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "listen.port") == 0) {
        if (parse_u16(val, &u16) != 0 || u16 == 0) {
            FAIL("invalid port");
        }
        c->listen_port = u16;
        return 0;
    }
    if (strcmp(key, "spa.root") == 0 || strcmp(key, "spa_root") == 0) {
        if (copy_str(c->spa_root, sizeof(c->spa_root), val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "packages.root") == 0 ||
        strcmp(key, "packages_root") == 0 ||
        strcmp(key, "spa.packages") == 0) {
        if (copy_str(c->packages_root, sizeof(c->packages_root), val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "spa.max_file_bytes") == 0 ||
        strcmp(key, "static.max_file_bytes") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0) {
            FAIL("invalid size");
        }
        c->static_max_file_bytes = sz;
        return 0;
    }
    if (strcmp(key, "http.max_body_bytes") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0) {
            FAIL("invalid size");
        }
        c->http_max_body_bytes = sz;
        return 0;
    }
    if (strcmp(key, "http.max_pending_outbound") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0) {
            FAIL("invalid size");
        }
        c->http_max_pending_outbound = sz;
        return 0;
    }
    if (strcmp(key, "http.max_upstream_body_bytes") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0) {
            FAIL("invalid size");
        }
        c->http_max_upstream_body_bytes = sz;
        return 0;
    }
    if (strcmp(key, "state.max_keys_default") == 0 ||
        strcmp(key, "state.max_keys_per_ns") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0) {
            FAIL("invalid size");
        }
        c->state_max_keys_default = sz;
        return 0;
    }
    if (strcmp(key, "state.max_value_bytes") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0) {
            FAIL("invalid size");
        }
        c->state_max_value_bytes = sz;
        return 0;
    }
    if (strcmp(key, "state.namespaces.net_core.enabled") == 0 ||
        strcmp(key, "state.net_core.enabled") == 0 ||
        strcmp(key, "state.enable_net_core") == 0) {
        if (parse_bool(val, &iv) != 0) {
            FAIL("invalid bool");
        }
        c->state_net_core_enabled = iv;
        return 0;
    }
    if (strcmp(key, "state.namespaces.map_dynamic.enabled") == 0 ||
        strcmp(key, "state.map_dynamic.enabled") == 0 ||
        strcmp(key, "state.enable_map_dynamic") == 0) {
        if (parse_bool(val, &iv) != 0) {
            FAIL("invalid bool");
        }
        c->state_map_dynamic_enabled = iv;
        return 0;
    }
    if (strcmp(key, "state.namespaces.net_pon.enabled") == 0 ||
        strcmp(key, "state.net_pon.enabled") == 0) {
        if (parse_bool(val, &iv) != 0) {
            FAIL("invalid bool");
        }
        c->state_net_pon_enabled = iv;
        return 0;
    }
    if (strcmp(key, "state.namespaces.net_home.enabled") == 0 ||
        strcmp(key, "state.net_home.enabled") == 0) {
        if (parse_bool(val, &iv) != 0) {
            FAIL("invalid bool");
        }
        c->state_net_home_enabled = iv;
        return 0;
    }
    if (strcmp(key, "state.namespaces.electric.enabled") == 0 ||
        strcmp(key, "state.electric.enabled") == 0) {
        if (parse_bool(val, &iv) != 0) {
            FAIL("invalid bool");
        }
        c->state_electric_enabled = iv;
        return 0;
    }
    if (strcmp(key, "state.namespaces.inventory.enabled") == 0 ||
        strcmp(key, "state.inventory.enabled") == 0) {
        if (parse_bool(val, &iv) != 0) {
            FAIL("invalid bool");
        }
        c->state_inventory_enabled = iv;
        return 0;
    }
    if (strcmp(key, "state.namespaces.net_core.max_keys") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0) {
            FAIL("invalid size");
        }
        c->state_net_core_max_keys = sz;
        return 0;
    }
    if (strcmp(key, "state.namespaces.map_dynamic.max_keys") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0) {
            FAIL("invalid size");
        }
        c->state_map_dynamic_max_keys = sz;
        return 0;
    }
    if (strcmp(key, "state.namespaces.net_pon.max_keys") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0) {
            FAIL("invalid size");
        }
        c->state_net_pon_max_keys = sz;
        return 0;
    }
    if (strcmp(key, "state.namespaces.net_home.max_keys") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0) {
            FAIL("invalid size");
        }
        c->state_net_home_max_keys = sz;
        return 0;
    }
    if (strcmp(key, "state.namespaces.electric.max_keys") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0) {
            FAIL("invalid size");
        }
        c->state_electric_max_keys = sz;
        return 0;
    }
    if (strcmp(key, "state.namespaces.inventory.max_keys") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0) {
            FAIL("invalid size");
        }
        c->state_inventory_max_keys = sz;
        return 0;
    }
    if (strcmp(key, "auth.mode") == 0) {
        if (copy_str(c->auth_mode, sizeof(c->auth_mode), val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "auth.lab_password_env") == 0) {
        if (copy_str(c->auth_lab_password_env, sizeof(c->auth_lab_password_env),
                     val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "auth.session_hmac_key_env") == 0) {
        if (copy_str(c->auth_session_hmac_key_env,
                     sizeof(c->auth_session_hmac_key_env), val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "auth.session_ttl_s") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0 || sz > 0xffffffffu) {
            FAIL("invalid ttl");
        }
        c->auth_session_ttl_s = (uint32_t)sz;
        return 0;
    }
    if (strcmp(key, "auth.proxy_hmac_key_env") == 0) {
        if (copy_str(c->auth_proxy_hmac_key_env,
                     sizeof(c->auth_proxy_hmac_key_env), val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "auth.proxy_max_skew_s") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0 || sz > 0xffffffffu) {
            FAIL("invalid skew");
        }
        c->auth_proxy_max_skew_s = (uint32_t)sz;
        return 0;
    }
    if (strcmp(key, "dns.allow_blocking") == 0) {
        if (parse_bool(val, &iv) != 0) {
            FAIL("invalid bool");
        }
        c->dns_allow_blocking = iv;
        return 0;
    }
    if (strcmp(key, "plugins.openai_proxy.enabled") == 0 ||
        strcmp(key, "openai_proxy.enabled") == 0) {
        if (parse_bool(val, &iv) != 0) {
            FAIL("invalid bool");
        }
        c->openai_enabled = iv;
        return 0;
    }
    if (strcmp(key, "plugins.openai_proxy.upstream") == 0 ||
        strcmp(key, "openai_proxy.upstream") == 0) {
        if (copy_str(c->openai_upstream, sizeof(c->openai_upstream), val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "plugins.openai_proxy.upstream_addr") == 0 ||
        strcmp(key, "openai_proxy.upstream_addr") == 0) {
        if (copy_str(c->openai_upstream_addr, sizeof(c->openai_upstream_addr),
                     val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "plugins.openai_proxy.upstream_host") == 0 ||
        strcmp(key, "openai_proxy.upstream_host") == 0) {
        if (copy_str(c->openai_upstream_host, sizeof(c->openai_upstream_host),
                     val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "plugins.openai_proxy.api_key_env") == 0) {
        if (copy_str(c->openai_api_key_env, sizeof(c->openai_api_key_env),
                     val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "plugins.openai_proxy.service_api_key_env") == 0) {
        if (copy_str(c->openai_service_key_env, sizeof(c->openai_service_key_env),
                     val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "plugins.openai_proxy.timeout_ms") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0 || sz > 0xffffffffu) {
            FAIL("invalid");
        }
        c->openai_timeout_ms = (uint32_t)sz;
        return 0;
    }
    if (strcmp(key, "plugins.openai_proxy.rate_limit_rpm") == 0) {
        if (parse_size(val, &sz) != 0 || sz > 0xffffffffu) {
            FAIL("invalid");
        }
        c->openai_rate_limit_rpm = (uint32_t)sz;
        return 0;
    }
    if (strcmp(key, "plugins.openai_proxy.max_concurrent_per_principal") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0 || sz > 0xffffffffu) {
            FAIL("invalid");
        }
        c->openai_max_concurrent = (uint32_t)sz;
        return 0;
    }
    if (strcmp(key, "plugins.slack.enabled") == 0 ||
        strcmp(key, "slack.enabled") == 0) {
        if (parse_bool(val, &iv) != 0) {
            FAIL("invalid bool");
        }
        c->slack_enabled = iv;
        return 0;
    }
    if (strcmp(key, "plugins.teams.enabled") == 0 ||
        strcmp(key, "teams.enabled") == 0) {
        if (parse_bool(val, &iv) != 0) {
            FAIL("invalid bool");
        }
        c->teams_enabled = iv;
        return 0;
    }
    if (strcmp(key, "tls.cert") == 0 || strcmp(key, "tls.cert_file") == 0) {
        if (copy_str(c->tls_cert, sizeof(c->tls_cert), val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "tls.key") == 0 || strcmp(key, "tls.key_file") == 0) {
        if (copy_str(c->tls_key, sizeof(c->tls_key), val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "tls.client_ca") == 0 ||
        strcmp(key, "tls.client_ca_file") == 0) {
        if (copy_str(c->tls_client_ca, sizeof(c->tls_client_ca), val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "plugins.pqproxy.enabled") == 0 ||
        strcmp(key, "pqproxy.enabled") == 0) {
        if (parse_bool(val, &iv) != 0) {
            FAIL("invalid bool");
        }
        c->pqproxy_enabled = iv;
        return 0;
    }
    if (strcmp(key, "plugins.pqproxy.metrics_url") == 0 ||
        strcmp(key, "pqproxy.metrics_url") == 0) {
        if (copy_str(c->pqproxy_metrics_url, sizeof(c->pqproxy_metrics_url),
                     val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "plugins.pqproxy.scrape_interval_ms") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0 || sz > 0xffffffffu) {
            FAIL("invalid");
        }
        c->pqproxy_scrape_interval_ms = (uint32_t)sz;
        return 0;
    }
    if (strcmp(key, "postgres.notify_enabled") == 0 ||
        strcmp(key, "postgres.enabled") == 0) {
        if (parse_bool(val, &iv) != 0) {
            FAIL("invalid bool");
        }
        c->postgres_notify_enabled = iv;
        return 0;
    }
    if (strcmp(key, "postgres.listen_channels") == 0 ||
        strcmp(key, "postgres.listen_channel") == 0) {
        /* single channel string or first list item as scalar */
        if (copy_str(c->postgres_listen_channel,
                     sizeof(c->postgres_listen_channel), val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    /* plugins.e7_callhome (PR-2) */
    if (strcmp(key, "plugins.e7_callhome.enabled") == 0 ||
        strcmp(key, "e7_callhome.enabled") == 0) {
        if (parse_bool(val, &iv) != 0) {
            FAIL("invalid bool");
        }
        c->e7_enabled = iv;
        return 0;
    }
    if (strcmp(key, "plugins.e7_callhome.listen_host") == 0 ||
        strcmp(key, "plugins.e7_callhome.listen.host") == 0) {
        if (copy_str(c->e7_listen_host, sizeof(c->e7_listen_host), val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "plugins.e7_callhome.listen_port") == 0 ||
        strcmp(key, "plugins.e7_callhome.listen.port") == 0) {
        if (parse_u16(val, &u16) != 0 || u16 == 0) {
            FAIL("invalid port");
        }
        c->e7_listen_port = u16;
        return 0;
    }
    if (strcmp(key, "plugins.e7_callhome.transport") == 0) {
        if (copy_str(c->e7_transport, sizeof(c->e7_transport), val) != 0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "plugins.e7_callhome.ssh_password") == 0) {
        if (copy_str(c->e7_ssh_password, sizeof(c->e7_ssh_password), val) !=
            0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "plugins.e7_callhome.ssh_username") == 0) {
        if (copy_str(c->e7_ssh_username, sizeof(c->e7_ssh_username), val) !=
            0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "plugins.e7_callhome.host_key_path") == 0) {
        if (copy_str(c->e7_host_key_path, sizeof(c->e7_host_key_path), val) !=
            0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "plugins.e7_callhome.ssh_allow_none_auth") == 0) {
        if (parse_bool(val, &iv) != 0) {
            FAIL("invalid bool");
        }
        c->e7_ssh_allow_none_auth = iv;
        return 0;
    }
    if (strcmp(key, "plugins.e7_callhome.lab_insecure_raw") == 0) {
        if (parse_bool(val, &iv) != 0) {
            FAIL("invalid bool");
        }
        c->e7_lab_insecure_raw = iv;
        return 0;
    }
    if (strcmp(key, "plugins.e7_callhome.reload_policy") == 0) {
        if (copy_str(c->e7_reload_policy, sizeof(c->e7_reload_policy), val) !=
            0) {
            FAIL("too long");
        }
        return 0;
    }
    if (strcmp(key, "plugins.e7_callhome.auto_subscribe_unknown") == 0) {
        if (parse_bool(val, &iv) != 0) {
            FAIL("invalid bool");
        }
        c->e7_auto_subscribe_unknown = iv;
        return 0;
    }
    if (strcmp(key, "plugins.e7_callhome.dirty_cap") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0 || sz > 0xffffffffu) {
            FAIL("invalid");
        }
        c->e7_dirty_cap = (uint32_t)sz;
        return 0;
    }
    if (strcmp(key, "plugins.e7_callhome.rss_budget_bytes") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0) {
            FAIL("invalid size");
        }
        c->e7_rss_budget_bytes = sz;
        return 0;
    }
    if (strcmp(key, "plugins.e7_callhome.allowlist_path") == 0) {
        if (copy_str(c->e7_allowlist_path, sizeof(c->e7_allowlist_path), val) !=
            0) {
            return -1;
        }
        return 1;
    }
    if (strcmp(key, "plugins.e7_callhome.max_sessions") == 0) {
        if (parse_size(val, &sz) != 0 || sz == 0 || sz > 0xffffffffu) {
            FAIL("invalid");
        }
        c->e7_max_sessions = (uint32_t)sz;
        return 0;
    }
#undef FAIL
    return 0; /* unknown keys ignored */
}

static const char *const g_paths[] = {
    "listen.host",
    "listen.port",
    "spa.root",
    "spa_root",
    "packages.root",
    "packages_root",
    "spa.packages",
    "spa.max_file_bytes",
    "static.max_file_bytes",
    "http.max_body_bytes",
    "http.max_pending_outbound",
    "http.max_upstream_body_bytes",
    "state.max_keys_default",
    "state.max_keys_per_ns",
    "state.max_value_bytes",
    "state.namespaces.net_core.enabled",
    "state.namespaces.map_dynamic.enabled",
    "state.namespaces.net_pon.enabled",
    "state.namespaces.net_home.enabled",
    "state.namespaces.electric.enabled",
    "state.namespaces.inventory.enabled",
    "state.namespaces.net_core.max_keys",
    "state.namespaces.map_dynamic.max_keys",
    "state.namespaces.net_pon.max_keys",
    "state.namespaces.net_home.max_keys",
    "state.namespaces.electric.max_keys",
    "state.namespaces.inventory.max_keys",
    "state.net_core.enabled",
    "state.map_dynamic.enabled",
    "state.net_pon.enabled",
    "state.net_home.enabled",
    "state.electric.enabled",
    "state.inventory.enabled",
    "state.enable_net_core",
    "state.enable_map_dynamic",
    "auth.mode",
    "auth.lab_password_env",
    "auth.session_hmac_key_env",
    "auth.session_ttl_s",
    "auth.proxy_hmac_key_env",
    "auth.proxy_max_skew_s",
    "dns.allow_blocking",
    "plugins.openai_proxy.enabled",
    "openai_proxy.enabled",
    "plugins.openai_proxy.upstream",
    "openai_proxy.upstream",
    "plugins.openai_proxy.upstream_addr",
    "openai_proxy.upstream_addr",
    "plugins.openai_proxy.upstream_host",
    "openai_proxy.upstream_host",
    "plugins.openai_proxy.api_key_env",
    "plugins.openai_proxy.service_api_key_env",
    "plugins.openai_proxy.timeout_ms",
    "plugins.openai_proxy.rate_limit_rpm",
    "plugins.openai_proxy.max_concurrent_per_principal",
    "plugins.slack.enabled",
    "slack.enabled",
    "plugins.teams.enabled",
    "teams.enabled",
    "tls.cert",
    "tls.cert_file",
    "tls.key",
    "tls.key_file",
    "tls.client_ca",
    "tls.client_ca_file",
    "plugins.pqproxy.enabled",
    "pqproxy.enabled",
    "plugins.pqproxy.metrics_url",
    "pqproxy.metrics_url",
    "plugins.pqproxy.scrape_interval_ms",
    "postgres.notify_enabled",
    "postgres.enabled",
    "postgres.listen_channels",
    "postgres.listen_channel",
    "plugins.e7_callhome.enabled",
    "e7_callhome.enabled",
    "plugins.e7_callhome.listen_host",
    "plugins.e7_callhome.listen.host",
    "plugins.e7_callhome.listen_port",
    "plugins.e7_callhome.listen.port",
    "plugins.e7_callhome.transport",
    "plugins.e7_callhome.ssh_password",
    "plugins.e7_callhome.ssh_username",
    "plugins.e7_callhome.host_key_path",
    "plugins.e7_callhome.ssh_allow_none_auth",
    "plugins.e7_callhome.lab_insecure_raw",
    "plugins.e7_callhome.reload_policy",
    "plugins.e7_callhome.auto_subscribe_unknown",
    "plugins.e7_callhome.dirty_cap",
    "plugins.e7_callhome.rss_budget_bytes",
    "plugins.e7_callhome.max_sessions",
    "plugins.e7_callhome.allowlist_path",
    NULL
};

/**
 * Load plugins.e7_callhome.shelves[] sequence (mac primary; optional
 * shelf_id/label; enabled default true). Paths: shelves.N.mac etc.
 */
static int load_e7_shelves(yaml_ctx_t *ctx, edge_config_t *c, char *err,
                           size_t err_len)
{
    size_t i;
    char path[96];
    const char *val;
    int iv;

    c->e7_shelf_count = 0;
    for (i = 0; i < EDGE_CONFIG_E7_SHELVES_MAX; i++) {
        edge_e7_shelf_config_t *s = &c->e7_shelves[i];

        snprintf(path, sizeof(path), "plugins.e7_callhome.shelves.%zu.mac", i);
        val = lookup(ctx, path);
        if (!val) {
            /* also accept top-level e7_callhome.shelves */
            snprintf(path, sizeof(path), "e7_callhome.shelves.%zu.mac", i);
            val = lookup(ctx, path);
        }
        if (!val) {
            break; /* end of dense sequence */
        }
        if (copy_str(s->mac, sizeof(s->mac), val) != 0) {
            if (err && err_len) {
                snprintf(err, err_len,
                         "plugins.e7_callhome.shelves[%zu].mac: too long", i);
            }
            return -1;
        }
        s->shelf_id[0] = '\0';
        s->enabled = 1;

        snprintf(path, sizeof(path),
                 "plugins.e7_callhome.shelves.%zu.shelf_id", i);
        val = lookup(ctx, path);
        if (!val) {
            snprintf(path, sizeof(path),
                     "plugins.e7_callhome.shelves.%zu.label", i);
            val = lookup(ctx, path);
        }
        if (!val) {
            snprintf(path, sizeof(path), "e7_callhome.shelves.%zu.shelf_id", i);
            val = lookup(ctx, path);
        }
        if (!val) {
            snprintf(path, sizeof(path), "e7_callhome.shelves.%zu.label", i);
            val = lookup(ctx, path);
        }
        if (val) {
            if (copy_str(s->shelf_id, sizeof(s->shelf_id), val) != 0) {
                if (err && err_len) {
                    snprintf(err, err_len,
                             "plugins.e7_callhome.shelves[%zu].shelf_id: "
                             "too long",
                             i);
                }
                return -1;
            }
        }

        snprintf(path, sizeof(path),
                 "plugins.e7_callhome.shelves.%zu.enabled", i);
        val = lookup(ctx, path);
        if (!val) {
            snprintf(path, sizeof(path), "e7_callhome.shelves.%zu.enabled", i);
            val = lookup(ctx, path);
        }
        if (val) {
            if (parse_bool(val, &iv) != 0) {
                if (err && err_len) {
                    snprintf(err, err_len,
                             "plugins.e7_callhome.shelves[%zu].enabled: "
                             "invalid bool",
                             i);
                }
                return -1;
            }
            s->enabled = iv;
        }

        c->e7_shelf_count = (uint32_t)(i + 1);
    }
    /* If denser than max, surface overflow when next index has mac */
    {
        snprintf(path, sizeof(path),
                 "plugins.e7_callhome.shelves.%zu.mac",
                 (size_t)EDGE_CONFIG_E7_SHELVES_MAX);
        val = lookup(ctx, path);
        if (!val) {
            snprintf(path, sizeof(path), "e7_callhome.shelves.%zu.mac",
                     (size_t)EDGE_CONFIG_E7_SHELVES_MAX);
            val = lookup(ctx, path);
        }
        if (val) {
            if (err && err_len) {
                snprintf(err, err_len,
                         "plugins.e7_callhome.shelves: exceeds max %d",
                         EDGE_CONFIG_E7_SHELVES_MAX);
            }
            return -1;
        }
    }
    return 0;
}

static int load_from_ctx(yaml_ctx_t *ctx, edge_config_t *c, char *err,
                         size_t err_len)
{
    size_t i;
    yaml_event_t ev;

    while (yaml_next_event(ctx, &ev)) {
        if (ev.type == YAML_EVENT_ERROR) {
            if (err && err_len) {
                snprintf(err, err_len, "yaml parse: %s",
                         ev.data.error.message[0] ? ev.data.error.message
                                                  : "error");
            }
            return -1;
        }
    }

    for (i = 0; g_paths[i]; i++) {
        const char *val = lookup(ctx, g_paths[i]);
        if (!val) {
            continue;
        }
        if (apply_scalar(c, g_paths[i], val, err, err_len) != 0) {
            return -1;
        }
    }
    if (load_e7_shelves(ctx, c, err, err_len) != 0) {
        return -1;
    }
    return 0;
}

int edge_config_load_yaml_buf(const char *yaml, size_t yaml_len,
                              edge_config_t *out, char *err, size_t err_len)
{
    yaml_ctx_t *ctx;
    size_t n;

    if (!out) {
        if (err && err_len) {
            snprintf(err, err_len, "null args");
        }
        return -1;
    }
    edge_config_defaults(out);
    if (!yaml || yaml_len == 0) {
        return 0;
    }

    ctx = yaml_create(YAML_ROLE_PARSER);
    if (!ctx) {
        if (err && err_len) {
            snprintf(err, err_len, "yaml_create failed");
        }
        return -1;
    }
    n = yaml_feed_input(ctx, (const uint8_t *)yaml, yaml_len);
    if (n == 0 && yaml_len > 0) {
        if (err && err_len) {
            snprintf(err, err_len, "yaml_feed_input consumed 0");
        }
        yaml_destroy(ctx);
        return -1;
    }
    if (load_from_ctx(ctx, out, err, err_len) != 0) {
        yaml_destroy(ctx);
        return -1;
    }
    yaml_destroy(ctx);
    return 0;
}

int edge_config_load_yaml_path(const char *path, edge_config_t *out, char *err,
                               size_t err_len)
{
    FILE *fp;
    char *buf = NULL;
    size_t cap = 0;
    size_t len = 0;
    char chunk[4096];
    size_t nr;
    int rc;

    if (!path || !out) {
        if (err && err_len) {
            snprintf(err, err_len, "null args");
        }
        return -1;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        if (err && err_len) {
            snprintf(err, err_len, "cannot open %s", path);
        }
        return -1;
    }
    while ((nr = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        char *nb;
        if (len + nr + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 8192;
            while (ncap < len + nr + 1) {
                ncap *= 2;
            }
            nb = (char *)realloc(buf, ncap);
            if (!nb) {
                free(buf);
                fclose(fp);
                if (err && err_len) {
                    snprintf(err, err_len, "oom reading config");
                }
                return -1;
            }
            buf = nb;
            cap = ncap;
        }
        memcpy(buf + len, chunk, nr);
        len += nr;
    }
    fclose(fp);
    if (!buf) {
        edge_config_defaults(out);
        return 0;
    }
    buf[len] = '\0';
    rc = edge_config_load_yaml_buf(buf, len, out, err, err_len);
    free(buf);
    return rc;
}
