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
    c->http_max_body_bytes = 1024u * 1024u;
    c->http_max_pending_outbound = 256;
    c->http_max_upstream_body_bytes = 4u * 1024u * 1024u;
    c->state_net_core_enabled = 1;
    c->state_map_dynamic_enabled = 1;
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
    return 0;
}
