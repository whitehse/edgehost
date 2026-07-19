/**
 * @file edge_teams_plugin.h
 * @brief Teams plugin stub (P1.10) — vtbl + config; no Graph/Bot I/O.
 */
#ifndef EDGE_TEAMS_PLUGIN_H
#define EDGE_TEAMS_PLUGIN_H

#include "edge_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  enabled; /* default 0 */
    char access_token_env[64];
    char tenant_id[128];
} edge_teams_config_t;

void edge_teams_config_defaults(edge_teams_config_t *c);

int edge_teams_init_plugin(edge_plugin_t *plugin, edge_teams_config_t *cfg_storage,
                           const edge_teams_config_t *cfg);

const edge_plugin_vtbl_t *edge_teams_vtbl(void);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_TEAMS_PLUGIN_H */
