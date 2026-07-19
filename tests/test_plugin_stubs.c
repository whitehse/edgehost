/**
 * P1.9 / P1.10: slack and teams SESSION stubs.
 */
#include "edge_config.h"
#include "edge_plugin_host.h"
#include "edge_slack_plugin.h"
#include "edge_state.h"
#include "edge_teams_plugin.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_slack_stub(void)
{
    edge_slack_config_t cfg, store;
    edge_plugin_t pl;
    edge_plugin_host_t *h;
    edge_plugin_host_config_t phc;
    edge_state_store_t *st = edge_state_create();

    edge_slack_config_defaults(&cfg);
    assert(cfg.enabled == 0);
    assert(edge_slack_init_plugin(&pl, &store, &cfg) == 0);
    assert(pl.vtbl && pl.vtbl->kind == EDGE_PLUGIN_KIND_SESSION);
    assert(strcmp(pl.vtbl->name, "slack") == 0);

    memset(&phc, 0, sizeof(phc));
    phc.state = st;
    h = edge_plugin_host_create(&phc);
    assert(h);
    assert(edge_plugin_host_register(h, &pl, NULL) == 0);
    assert(pl.vtbl->feed(&pl, (const uint8_t *)"x", 1) == 0);
    assert(pl.vtbl->next_event(&pl, NULL) == 0);

    edge_plugin_host_destroy(h);
    edge_state_destroy(st);
    printf("  PASS: slack stub\n");
}

static void test_teams_stub(void)
{
    edge_teams_config_t cfg, store;
    edge_plugin_t pl;
    edge_plugin_host_t *h;
    edge_plugin_host_config_t phc;
    edge_state_store_t *st = edge_state_create();

    edge_teams_config_defaults(&cfg);
    assert(cfg.enabled == 0);
    assert(edge_teams_init_plugin(&pl, &store, &cfg) == 0);
    assert(pl.vtbl->kind == EDGE_PLUGIN_KIND_SESSION);
    assert(strcmp(pl.vtbl->name, "teams") == 0);

    memset(&phc, 0, sizeof(phc));
    phc.state = st;
    h = edge_plugin_host_create(&phc);
    assert(edge_plugin_host_register(h, &pl, NULL) == 0);

    edge_plugin_host_destroy(h);
    edge_state_destroy(st);
    printf("  PASS: teams stub\n");
}

static void test_yaml_flags(void)
{
    edge_config_t c;
    char err[64];
    const char *yaml =
        "plugins:\n"
        "  slack:\n"
        "    enabled: true\n"
        "  teams:\n"
        "    enabled: false\n"
        "tls:\n"
        "  cert: /tmp/c.pem\n"
        "  key: /tmp/k.pem\n";
    assert(edge_config_load_yaml_buf(yaml, strlen(yaml), &c, err, sizeof(err)) ==
           0);
    assert(c.slack_enabled == 1);
    assert(c.teams_enabled == 0);
    assert(strcmp(c.tls_cert, "/tmp/c.pem") == 0);
    assert(strcmp(c.tls_key, "/tmp/k.pem") == 0);
    printf("  PASS: stub/tls yaml\n");
}

int main(void)
{
    printf("edgehost_stubs_test:\n");
    test_slack_stub();
    test_teams_stub();
    test_yaml_flags();
    printf("All stub tests PASSED\n");
    return 0;
}
