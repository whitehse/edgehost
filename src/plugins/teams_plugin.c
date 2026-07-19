/**
 * @file teams_plugin.c
 * @brief Teams SESSION plugin stub (P1.10). enabled:false by default.
 */

#include "edge_teams_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    edge_teams_config_t cfg;
} teams_data_t;

void edge_teams_config_defaults(edge_teams_config_t *c)
{
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->enabled = 0;
    snprintf(c->access_token_env, sizeof(c->access_token_env),
             "TEAMS_ACCESS_TOKEN");
}

static int teams_init(edge_plugin_t *self, const edge_host_api_t *host,
                      const void *cfg_opaque)
{
    teams_data_t *d = (teams_data_t *)self->user_data;
    (void)cfg_opaque;
    if (d && d->cfg.enabled && host && host->log) {
        host->log(host->ctx, EDGE_LOG_INFO,
                  "teams_plugin: enabled but stub (phase-2 Graph/Bot)");
    }
    return 0;
}

static void teams_shutdown(edge_plugin_t *self)
{
    if (self && self->user_data) {
        free(self->user_data);
        self->user_data = NULL;
    }
}

static int teams_feed(edge_plugin_t *self, const uint8_t *data, size_t len)
{
    (void)self;
    (void)data;
    (void)len;
    return 0;
}

static int teams_next_event(edge_plugin_t *self, void *ev_out)
{
    (void)self;
    (void)ev_out;
    return 0;
}

static int teams_on_tick(edge_plugin_t *self, uint64_t mono_ms)
{
    (void)self;
    (void)mono_ms;
    return 0;
}

static const edge_plugin_vtbl_t teams_vtbl = {
    .name = "teams",
    .version = "0.1-stub",
    .kind = EDGE_PLUGIN_KIND_SESSION,
    .init = teams_init,
    .shutdown = teams_shutdown,
    .on_config_reload = NULL,
    .on_http = NULL,
    .on_http_complete = NULL,
    .feed = teams_feed,
    .next_event = teams_next_event,
    .on_tick = teams_on_tick,
};

const edge_plugin_vtbl_t *edge_teams_vtbl(void)
{
    return &teams_vtbl;
}

int edge_teams_init_plugin(edge_plugin_t *plugin, edge_teams_config_t *cfg_storage,
                           const edge_teams_config_t *cfg)
{
    teams_data_t *d;

    if (!plugin || !cfg_storage) {
        return -1;
    }
    if (cfg) {
        *cfg_storage = *cfg;
    } else {
        edge_teams_config_defaults(cfg_storage);
    }
    d = (teams_data_t *)calloc(1, sizeof(*d));
    if (!d) {
        return -1;
    }
    d->cfg = *cfg_storage;
    memset(plugin, 0, sizeof(*plugin));
    plugin->vtbl = &teams_vtbl;
    plugin->user_data = d;
    return 0;
}
