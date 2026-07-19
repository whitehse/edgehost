/**
 * @file auth_host.c
 * @brief Resolve auth secrets from process environment (P1.7c).
 */

#include "edge_auth_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int edge_auth_ctx_from_config(const edge_config_t *cfg, edge_auth_ctx_t *out,
                              char *err, size_t err_len)
{
    const char *mode;
    const char *pw_env;
    const char *key_env;
    const char *pw;
    const char *key;

    if (!cfg || !out) {
        if (err && err_len) {
            snprintf(err, err_len, "null args");
        }
        return -1;
    }
    edge_auth_ctx_init(out);
    mode = cfg->auth_mode[0] ? cfg->auth_mode : "open";
    if (strcmp(mode, "open") == 0) {
        out->mode = EDGE_AUTH_MODE_OPEN;
        out->session_ttl_s =
            cfg->auth_session_ttl_s ? cfg->auth_session_ttl_s : 28800;
        return 0;
    }
    if (strcmp(mode, "proxy_headers") == 0) {
        /* P1.7d: accept mode in config but not yet implemented. */
        if (err && err_len) {
            snprintf(err, err_len,
                     "auth.mode=proxy_headers not implemented (P1.7d)");
        }
        return -1;
    }
    if (strcmp(mode, "lab_password") != 0) {
        if (err && err_len) {
            snprintf(err, err_len, "unknown auth.mode");
        }
        return -1;
    }

    out->mode = EDGE_AUTH_MODE_LAB_PASSWORD;
    out->session_ttl_s =
        cfg->auth_session_ttl_s ? cfg->auth_session_ttl_s : 28800;

    pw_env = cfg->auth_lab_password_env[0] ? cfg->auth_lab_password_env
                                           : "EDGEHOST_LAB_PASSWORD";
    key_env = cfg->auth_session_hmac_key_env[0]
                  ? cfg->auth_session_hmac_key_env
                  : "EDGEHOST_SESSION_HMAC";

    pw = getenv(pw_env);
    key = getenv(key_env);
    if (!pw || !pw[0]) {
        if (err && err_len) {
            snprintf(err, err_len, "env %s empty (lab password)", pw_env);
        }
        return -1;
    }
    if (!key || !key[0]) {
        if (err && err_len) {
            snprintf(err, err_len, "env %s empty (session HMAC key)", key_env);
        }
        return -1;
    }
    if (strlen(pw) >= EDGE_AUTH_PASSWORD_MAX) {
        if (err && err_len) {
            snprintf(err, err_len, "lab password too long");
        }
        return -1;
    }
    if (strlen(key) > EDGE_AUTH_HMAC_KEY_MAX) {
        if (err && err_len) {
            snprintf(err, err_len, "session HMAC key too long");
        }
        return -1;
    }
    snprintf(out->lab_password, sizeof(out->lab_password), "%s", pw);
    out->hmac_key_len = strlen(key);
    memcpy(out->hmac_key, key, out->hmac_key_len);
    return 0;
}
