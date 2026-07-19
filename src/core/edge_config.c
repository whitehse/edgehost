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
    c->generation = 0;
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
    return 0;
}
