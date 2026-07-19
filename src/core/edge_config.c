/**
 * @file edge_config.c
 * @brief edge_config defaults + validate (syscall-free; shared by core + host).
 */

#include "edge_config.h"

#include <stdio.h>
#include <string.h>

void edge_config_defaults(edge_config_t *c)
{
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
    snprintf(c->listen_host, sizeof(c->listen_host), "0.0.0.0");
    c->listen_port = 8080;
    snprintf(c->spa_root, sizeof(c->spa_root), "./spa");
    snprintf(c->packages_root, sizeof(c->packages_root), "./packages");
    c->static_max_file_bytes = 64u * 1024u;
    c->http_max_body_bytes = 1024u * 1024u;
    c->http_max_pending_outbound = 256;
    c->http_max_upstream_body_bytes = 4u * 1024u * 1024u;
    c->state_net_core_enabled = 1;
    c->state_map_dynamic_enabled = 1;
    c->state_net_pon_enabled = 0;
    c->state_net_home_enabled = 0;
    c->state_electric_enabled = 0;
    c->state_inventory_enabled = 0;
    snprintf(c->auth_mode, sizeof(c->auth_mode), "open");
    snprintf(c->auth_lab_password_env, sizeof(c->auth_lab_password_env),
             "EDGEHOST_LAB_PASSWORD");
    snprintf(c->auth_session_hmac_key_env, sizeof(c->auth_session_hmac_key_env),
             "EDGEHOST_SESSION_HMAC");
    snprintf(c->auth_proxy_hmac_key_env, sizeof(c->auth_proxy_hmac_key_env),
             "EDGEHOST_PROXY_HMAC");
    c->auth_session_ttl_s = 28800;
    c->auth_proxy_max_skew_s = 300;
    c->dns_allow_blocking = 0;
    c->openai_enabled = 0;
    snprintf(c->openai_upstream, sizeof(c->openai_upstream),
             "https://api.openai.com");
    snprintf(c->openai_api_key_env, sizeof(c->openai_api_key_env),
             "OPENAI_API_KEY");
    snprintf(c->openai_service_key_env, sizeof(c->openai_service_key_env),
             "EDGEHOST_OPENAI_SERVICE_KEY");
    c->openai_timeout_ms = 60000;
    c->openai_rate_limit_rpm = 60;
    c->openai_max_concurrent = 4;
    c->slack_enabled = 0;
    c->teams_enabled = 0;
    c->tls_cert[0] = '\0';
    c->tls_key[0] = '\0';
    c->tls_client_ca[0] = '\0';
    c->pqproxy_enabled = 0;
    snprintf(c->pqproxy_metrics_url, sizeof(c->pqproxy_metrics_url),
             "http://127.0.0.1:9108/metrics");
    c->pqproxy_scrape_interval_ms = 5000;
    c->postgres_notify_enabled = 0;
    snprintf(c->postgres_listen_channel, sizeof(c->postgres_listen_channel),
             "map_overlay");
    /* E7 Call Home — disabled; lab-safe defaults (PR-2). */
    c->e7_enabled = 0;
    snprintf(c->e7_listen_host, sizeof(c->e7_listen_host), "127.0.0.1");
    c->e7_listen_port = 4334;
    snprintf(c->e7_transport, sizeof(c->e7_transport), "raw");
    c->e7_lab_insecure_raw = 0;
    snprintf(c->e7_reload_policy, sizeof(c->e7_reload_policy), "merge");
    c->e7_auto_subscribe_unknown = 0;
    c->e7_dirty_cap = 8192;
    c->e7_rss_budget_bytes = 268435456u; /* 256 MiB */
    c->e7_max_sessions = 160;
    c->e7_shelf_count = 0;
    c->generation = 0;
}

/** True if host is a loopback name/address (IPv4/IPv6 localhost). */
static int e7_host_is_loopback(const char *host)
{
    if (!host || host[0] == '\0') {
        return 0;
    }
    if (strcmp(host, "127.0.0.1") == 0 || strcmp(host, "::1") == 0 ||
        strcmp(host, "localhost") == 0) {
        return 1;
    }
    /* 127.0.0.0/8 */
    if (strncmp(host, "127.", 4) == 0) {
        return 1;
    }
    return 0;
}

int edge_config_validate(const edge_config_t *c, char *err, size_t err_len)
{
    if (!c) {
        if (err && err_len) {
            snprintf(err, err_len, "null config");
        }
        return -1;
    }
    if (c->listen_host[0] == '\0') {
        if (err && err_len) {
            snprintf(err, err_len, "listen.host empty");
        }
        return -1;
    }
    if (c->listen_port == 0) {
        if (err && err_len) {
            snprintf(err, err_len, "listen.port invalid");
        }
        return -1;
    }
    if (c->http_max_body_bytes == 0) {
        if (err && err_len) {
            snprintf(err, err_len, "http.max_body_bytes must be > 0");
        }
        return -1;
    }
    if (c->http_max_pending_outbound == 0) {
        if (err && err_len) {
            snprintf(err, err_len, "http.max_pending_outbound must be > 0");
        }
        return -1;
    }
    if (c->http_max_upstream_body_bytes == 0) {
        if (err && err_len) {
            snprintf(err, err_len, "http.max_upstream_body_bytes must be > 0");
        }
        return -1;
    }
    if (c->auth_mode[0] != '\0' &&
        strcmp(c->auth_mode, "open") != 0 &&
        strcmp(c->auth_mode, "lab_password") != 0 &&
        strcmp(c->auth_mode, "proxy_headers") != 0) {
        if (err && err_len) {
            snprintf(err, err_len, "auth.mode invalid (open|lab_password|proxy_headers)");
        }
        return -1;
    }
    if (c->auth_session_ttl_s == 0) {
        if (err && err_len) {
            snprintf(err, err_len, "auth.session_ttl_s must be > 0");
        }
        return -1;
    }
    if (c->auth_proxy_max_skew_s == 0) {
        if (err && err_len) {
            snprintf(err, err_len, "auth.proxy_max_skew_s must be > 0");
        }
        return -1;
    }
    /* E7 Call Home structural + safety checks (even when disabled). */
    if (c->e7_listen_host[0] == '\0') {
        if (err && err_len) {
            snprintf(err, err_len, "plugins.e7_callhome.listen_host empty");
        }
        return -1;
    }
    if (c->e7_listen_port == 0) {
        if (err && err_len) {
            snprintf(err, err_len, "plugins.e7_callhome.listen_port invalid");
        }
        return -1;
    }
    if (strcmp(c->e7_transport, "raw") != 0 &&
        strcmp(c->e7_transport, "ssh") != 0) {
        if (err && err_len) {
            snprintf(err, err_len,
                     "plugins.e7_callhome.transport invalid (raw|ssh)");
        }
        return -1;
    }
    if (strcmp(c->e7_reload_policy, "merge") != 0 &&
        strcmp(c->e7_reload_policy, "replace_all") != 0) {
        if (err && err_len) {
            snprintf(err, err_len,
                     "plugins.e7_callhome.reload_policy invalid "
                     "(merge|replace_all)");
        }
        return -1;
    }
    if (c->e7_max_sessions == 0) {
        if (err && err_len) {
            snprintf(err, err_len,
                     "plugins.e7_callhome.max_sessions must be > 0");
        }
        return -1;
    }
    if (c->e7_rss_budget_bytes == 0) {
        if (err && err_len) {
            snprintf(err, err_len,
                     "plugins.e7_callhome.rss_budget_bytes must be > 0");
        }
        return -1;
    }
    if (c->e7_dirty_cap == 0) {
        if (err && err_len) {
            snprintf(err, err_len, "plugins.e7_callhome.dirty_cap must be > 0");
        }
        return -1;
    }
    if (c->e7_shelf_count > EDGE_CONFIG_E7_SHELVES_MAX) {
        if (err && err_len) {
            snprintf(err, err_len, "plugins.e7_callhome.shelves overflow");
        }
        return -1;
    }
    {
        uint32_t i;
        for (i = 0; i < c->e7_shelf_count; i++) {
            if (c->e7_shelves[i].mac[0] == '\0') {
                if (err && err_len) {
                    snprintf(err, err_len,
                             "plugins.e7_callhome.shelves[%u].mac required",
                             (unsigned)i);
                }
                return -1;
            }
        }
    }
    /* raw + non-loopback requires explicit lab_insecure_raw: true */
    if (strcmp(c->e7_transport, "raw") == 0 &&
        !e7_host_is_loopback(c->e7_listen_host) && !c->e7_lab_insecure_raw) {
        if (err && err_len) {
            snprintf(err, err_len,
                     "plugins.e7_callhome: transport=raw on non-loopback "
                     "requires lab_insecure_raw: true");
        }
        return -1;
    }
    return 0;
}
