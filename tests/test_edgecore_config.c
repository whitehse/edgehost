/**
 * P1.3: YAML load, apply_config, SIGHUP shadow reload.
 */
#include "edge_config.h"
#include "edge_config_hup.h"
#include "edgecore.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *example_yaml_path(void)
{
    /* Prefer source-tree example when run from build/ */
    static const char *candidates[] = {
        "config/edgehost.example.yaml",
        "../config/edgehost.example.yaml",
        NULL
    };
    size_t i;
    for (i = 0; candidates[i]; i++) {
        if (access(candidates[i], R_OK) == 0) {
            return candidates[i];
        }
    }
    return NULL;
}

static void test_defaults_validate(void)
{
    edge_config_t c;
    char err[64];

    edge_config_defaults(&c);
    assert(edge_config_validate(&c, err, sizeof(err)) == 0);
    assert(c.listen_port == 8080);
    assert(c.state_net_core_enabled == 1);
    assert(c.state_map_dynamic_enabled == 1);
    assert(c.state_net_pon_enabled == 0);
    assert(c.state_net_home_enabled == 0);
    assert(c.state_electric_enabled == 0);
    assert(c.state_inventory_enabled == 0);

    c.listen_port = 0;
    assert(edge_config_validate(&c, err, sizeof(err)) == -1);
    printf("  PASS: defaults + validate\n");
}

static void test_load_buf_and_apply(void)
{
    const char *yaml =
        "listen:\n"
        "  host: 127.0.0.1\n"
        "  port: 9090\n"
        "spa:\n"
        "  root: /var/www/spa\n"
        "http:\n"
        "  max_body_bytes: 2048\n"
        "  max_pending_outbound: 16\n"
        "  max_upstream_body_bytes: 4096\n"
        "state:\n"
        "  namespaces:\n"
        "    net_core:\n"
        "      enabled: true\n"
        "    map_dynamic:\n"
        "      enabled: false\n"
        "    net_pon:\n"
        "      enabled: true\n"
        "    net_home:\n"
        "      enabled: false\n"
        "    electric:\n"
        "      enabled: true\n"
        "    inventory:\n"
        "      enabled: true\n";
    edge_config_t cfg;
    edgecore_t *core;
    edge_event_t ev;
    char err[128];

    assert(edge_config_load_yaml_buf(yaml, strlen(yaml), &cfg, err,
                                     sizeof(err)) == 0);
    assert(strcmp(cfg.listen_host, "127.0.0.1") == 0);
    assert(cfg.listen_port == 9090);
    assert(strcmp(cfg.spa_root, "/var/www/spa") == 0);
    assert(cfg.http_max_body_bytes == 2048);
    assert(cfg.http_max_pending_outbound == 16);
    assert(cfg.http_max_upstream_body_bytes == 4096);
    assert(cfg.state_net_core_enabled == 1);
    assert(cfg.state_map_dynamic_enabled == 0);
    assert(cfg.state_net_pon_enabled == 1);
    assert(cfg.state_net_home_enabled == 0);
    assert(cfg.state_electric_enabled == 1);
    assert(cfg.state_inventory_enabled == 1);

    core = edgecore_create();
    assert(core);
    assert(edgecore_apply_config(core, &cfg) == 0);
    assert(edgecore_next_event(core, &ev) == 1);
    assert(ev.type == EDGE_EVENT_CONFIG_APPLIED);
    assert(edgecore_config(core)->generation == 1);
    assert(edgecore_config(core)->listen_port == 9090);
    assert(edgecore_config(core)->state_map_dynamic_enabled == 0);
    assert(edgecore_config(core)->state_net_pon_enabled == 1);

    edgecore_destroy(core);
    printf("  PASS: load buf + apply CONFIG_APPLIED\n");
}

static void test_reject_keeps_previous(void)
{
    edgecore_t *core = edgecore_create();
    edge_config_t good;
    edge_config_t bad;
    edge_event_t ev;
    uint64_t gen;

    edge_config_defaults(&good);
    good.listen_port = 8081;
    assert(edgecore_apply_config(core, &good) == 0);
    assert(edgecore_next_event(core, &ev) == 1);
    assert(ev.type == EDGE_EVENT_CONFIG_APPLIED);
    gen = edgecore_config(core)->generation;
    assert(gen == 1);

    edge_config_defaults(&bad);
    bad.listen_port = 0; /* invalid */
    assert(edgecore_apply_config(core, &bad) == -1);
    assert(edgecore_next_event(core, &ev) == 1);
    assert(ev.type == EDGE_EVENT_CONFIG_REJECTED);
    assert(edgecore_config(core)->generation == gen);
    assert(edgecore_config(core)->listen_port == 8081);

    edgecore_destroy(core);
    printf("  PASS: reject keeps previous\n");
}

static void test_load_example_path(void)
{
    const char *path = example_yaml_path();
    edge_config_t cfg;
    char err[128];

    if (!path) {
        printf("  SKIP: example yaml not found from cwd\n");
        return;
    }
    assert(edge_config_load_yaml_path(path, &cfg, err, sizeof(err)) == 0);
    assert(cfg.listen_port == 8080);
    assert(cfg.state_net_core_enabled == 1);
    assert(cfg.state_map_dynamic_enabled == 1);
    assert(cfg.state_net_pon_enabled == 0);
    assert(cfg.state_net_home_enabled == 0);
    assert(cfg.state_electric_enabled == 0);
    assert(cfg.state_inventory_enabled == 0);
    printf("  PASS: load example path (%s)\n", path);
}

static void test_reload_and_hup(void)
{
    const char *path = example_yaml_path();
    edgecore_t *core;
    edge_event_t ev;
    char err[128];
    edge_config_t first;

    if (!path) {
        printf("  SKIP: reload/hup (no example path)\n");
        return;
    }

    core = edgecore_create();
    assert(core);

    /* First apply via reload */
    assert(edgehost_reload_config(core, path, err, sizeof(err)) == 0);
    assert(edgecore_next_event(core, &ev) == 1);
    assert(ev.type == EDGE_EVENT_CONFIG_APPLIED);
    first = *edgecore_config(core);
    assert(first.generation == 1);

    /* Bad path → reject event, keep previous */
    assert(edgehost_reload_config(core, "/no/such/edgehost.yaml", err,
                                  sizeof(err)) == -1);
    assert(edgecore_next_event(core, &ev) == 1);
    assert(ev.type == EDGE_EVENT_CONFIG_REJECTED);
    assert(edgecore_config(core)->generation == first.generation);
    assert(edgecore_config(core)->listen_port == first.listen_port);

    /* SIGHUP flag */
    edgehost_hup_install();
    assert(edgehost_hup_take() == 0);
    raise(SIGHUP);
    assert(edgehost_hup_take() == 1);
    assert(edgehost_hup_take() == 0);

    assert(edgehost_reload_config(core, path, err, sizeof(err)) == 0);
    assert(edgecore_next_event(core, &ev) == 1);
    assert(ev.type == EDGE_EVENT_CONFIG_APPLIED);
    assert(edgecore_config(core)->generation == 2);

    edgecore_destroy(core);
    printf("  PASS: reload + SIGHUP flag\n");
}

int main(void)
{
    printf("edgecore_config:\n");
    test_defaults_validate();
    test_load_buf_and_apply();
    test_reject_keeps_previous();
    test_load_example_path();
    test_reload_and_hup();
    printf("all passed\n");
    return 0;
}
