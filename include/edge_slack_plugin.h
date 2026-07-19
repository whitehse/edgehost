/**
 * @file edge_slack_plugin.h
 * @brief Slack plugin stub (P1.9) — vtbl + config; no Socket Mode I/O.
 */
#ifndef EDGE_SLACK_PLUGIN_H
#define EDGE_SLACK_PLUGIN_H

#include "edge_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  enabled; /* default 0; phase-1 stays false */
    char bot_token_env[64];
    char app_token_env[64];
} edge_slack_config_t;

void edge_slack_config_defaults(edge_slack_config_t *c);

/**
 * Init plugin object. When enabled==0, init is a no-op success.
 * When enabled==1, still a stub (SESSION feed/next_event empty) until phase 2.
 */
int edge_slack_init_plugin(edge_plugin_t *plugin, edge_slack_config_t *cfg_storage,
                           const edge_slack_config_t *cfg);

const edge_plugin_vtbl_t *edge_slack_vtbl(void);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_SLACK_PLUGIN_H */
