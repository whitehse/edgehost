/**
 * @file slack_plugin.c
 * @brief Slack SESSION plugin stub (P1.9). enabled:false by default.
 *
 * Phase-2 will drive libslack Socket Mode via host API; this PR only
 * freezes the vtbl + config parse surface (no fake completeness).
 */

#include "edge_slack_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    edge_slack_config_t cfg;
} slack_data_t;

void edge_slack_config_defaults(edge_slack_config_t *c)
{
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->enabled = 0;
    snprintf(c->bot_token_env, sizeof(c->bot_token_env), "SLACK_BOT_TOKEN");
    snprintf(c->app_token_env, sizeof(c->app_token_env), "SLACK_APP_TOKEN");
}

static int slack_init(edge_plugin_t *self, const edge_host_api_t *host,
                      const void *cfg_opaque)
{
    slack_data_t *d = (slack_data_t *)self->user_data;
    (void)host;
    (void)cfg_opaque;
    if (d && d->cfg.enabled && host && host->log) {
        host->log(host->ctx, EDGE_LOG_INFO,
                  "slack_plugin: enabled but stub (phase-2 Socket Mode)");
    }
    return 0;
}

static void slack_shutdown(edge_plugin_t *self)
{
    if (self && self->user_data) {
        free(self->user_data);
        self->user_data = NULL;
    }
}

static int slack_feed(edge_plugin_t *self, const uint8_t *data, size_t len)
{
    (void)self;
    (void)data;
    (void)len;
    return 0; /* stub: consume nothing */
}

static int slack_next_event(edge_plugin_t *self, void *ev_out)
{
    (void)self;
    (void)ev_out;
    return 0; /* no events */
}

static int slack_on_tick(edge_plugin_t *self, uint64_t mono_ms)
{
    (void)self;
    (void)mono_ms;
    return 0;
}

static const edge_plugin_vtbl_t slack_vtbl = {
    .name = "slack",
    .version = "0.1-stub",
    .kind = EDGE_PLUGIN_KIND_SESSION,
    .init = slack_init,
    .shutdown = slack_shutdown,
    .on_config_reload = NULL,
    .on_http = NULL,
    .on_http_complete = NULL,
    .feed = slack_feed,
    .next_event = slack_next_event,
    .on_tick = slack_on_tick,
};

const edge_plugin_vtbl_t *edge_slack_vtbl(void)
{
    return &slack_vtbl;
}

int edge_slack_init_plugin(edge_plugin_t *plugin, edge_slack_config_t *cfg_storage,
                           const edge_slack_config_t *cfg)
{
    slack_data_t *d;

    if (!plugin || !cfg_storage) {
        return -1;
    }
    if (cfg) {
        *cfg_storage = *cfg;
    } else {
        edge_slack_config_defaults(cfg_storage);
    }
    d = (slack_data_t *)calloc(1, sizeof(*d));
    if (!d) {
        return -1;
    }
    d->cfg = *cfg_storage;
    memset(plugin, 0, sizeof(*plugin));
    plugin->vtbl = &slack_vtbl;
    plugin->user_data = d;
    return 0;
}
