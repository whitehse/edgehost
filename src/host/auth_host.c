/**
 * @file auth_host.c
 * @brief Resolve auth secrets from process environment (P1.7c / P1.7d).
 */

#include "edge_auth_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int load_key_env(const char *env_name, uint8_t *dst, size_t dst_max,
                        size_t *out_len, char *err, size_t err_len,
                        const char *label)
{
    const char *key = getenv(env_name);
    size_t n;

    if (!key || !key[0]) {
        if (err && err_len) {
            snprintf(err, err_len, "env %s empty (%s)", env_name, label);
        }
        return -1;
    }
    n = strlen(key);
    if (n > dst_max) {
        if (err && err_len) {
            snprintf(err, err_len, "env %s too long (%s)", env_name, label);
        }
        return -1;
    }
    memcpy(dst, key, n);
    *out_len = n;
    return 0;
}

int edge_auth_ctx_from_config(const edge_config_t *cfg, edge_auth_ctx_t *out,
                              char *err, size_t err_len)
{
    const char *mode;
    const char *pw_env;
    const char *key_env;
    const char *proxy_env;
    const char *pw;

    if (!cfg || !out) {
        if (err && err_len) {
            snprintf(err, err_len, "null args");
        }
        return -1;
    }
    edge_auth_ctx_init(out);
    out->session_ttl_s =
        cfg->auth_session_ttl_s ? cfg->auth_session_ttl_s : 28800;
    out->proxy_max_skew_s =
        cfg->auth_proxy_max_skew_s ? cfg->auth_proxy_max_skew_s : 300;

    mode = cfg->auth_mode[0] ? cfg->auth_mode : "open";
    if (strcmp(mode, "open") == 0) {
        out->mode = EDGE_AUTH_MODE_OPEN;
        return 0;
    }

    if (strcmp(mode, "proxy_headers") == 0) {
        out->mode = EDGE_AUTH_MODE_PROXY_HEADERS;
        proxy_env = cfg->auth_proxy_hmac_key_env[0]
                        ? cfg->auth_proxy_hmac_key_env
                        : "EDGEHOST_PROXY_HMAC";
        return load_key_env(proxy_env, out->proxy_hmac_key,
                            EDGE_AUTH_HMAC_KEY_MAX, &out->proxy_hmac_key_len, err,
                            err_len, "proxy HMAC key");
    }

    if (strcmp(mode, "lab_password") != 0) {
        if (err && err_len) {
            snprintf(err, err_len, "unknown auth.mode");
        }
        return -1;
    }

    out->mode = EDGE_AUTH_MODE_LAB_PASSWORD;

    pw_env = cfg->auth_lab_password_env[0] ? cfg->auth_lab_password_env
                                           : "EDGEHOST_LAB_PASSWORD";
    key_env = cfg->auth_session_hmac_key_env[0]
                  ? cfg->auth_session_hmac_key_env
                  : "EDGEHOST_SESSION_HMAC";

    pw = getenv(pw_env);
    if (!pw || !pw[0]) {
        if (err && err_len) {
            snprintf(err, err_len, "env %s empty (lab password)", pw_env);
        }
        return -1;
    }
    if (strlen(pw) >= EDGE_AUTH_PASSWORD_MAX) {
        if (err && err_len) {
            snprintf(err, err_len, "lab password too long");
        }
        return -1;
    }
    snprintf(out->lab_password, sizeof(out->lab_password), "%s", pw);
    return load_key_env(key_env, out->hmac_key, EDGE_AUTH_HMAC_KEY_MAX,
                        &out->hmac_key_len, err, err_len, "session HMAC key");
}
