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
    assert(c.state_max_keys_default == 0);
    assert(c.state_max_value_bytes == 0);
    assert(c.state_net_pon_max_keys == 0);
    assert(c.state_inventory_max_keys == 0);
    /* E7 Call Home lab-safe defaults (PR-2) */
    assert(c.e7_enabled == 0);
    assert(c.e7_listen_port == 4334);
    assert(strcmp(c.e7_listen_host, "127.0.0.1") == 0);
    assert(strcmp(c.e7_transport, "raw") == 0);
    assert(c.e7_lab_insecure_raw == 0);
    assert(strcmp(c.e7_reload_policy, "merge") == 0);
    assert(c.e7_auto_subscribe_unknown == 0);
    assert(c.e7_dirty_cap == 8192);
    assert(c.e7_rss_budget_bytes == 268435456u);
    assert(c.e7_max_sessions == 160);
    assert(c.e7_shelf_count == 0);

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

static void test_e7_callhome_yaml(void)
{
    const char *yaml =
        "plugins:\n"
        "  e7_callhome:\n"
        "    enabled: false\n"
        "    listen_host: 127.0.0.1\n"
        "    listen_port: 4334\n"
        "    transport: raw\n"
        "    lab_insecure_raw: false\n"
        "    reload_policy: merge\n"
        "    auto_subscribe_unknown: false\n"
        "    dirty_cap: 4096\n"
        "    rss_budget_bytes: 134217728\n"
        "    max_sessions: 32\n"
        "    shelves:\n"
        "      - mac: \"00:02:5d:d9:21:47\"\n"
        "        shelf_id: lab-e7-1\n"
        "        enabled: true\n"
        "      - mac: \"aa:bb:cc:dd:ee:ff\"\n"
        "        label: spare\n"
        "        enabled: false\n";
    edge_config_t cfg;
    char err[160];

    assert(edge_config_load_yaml_buf(yaml, strlen(yaml), &cfg, err,
                                     sizeof(err)) == 0);
    assert(cfg.e7_enabled == 0);
    assert(strcmp(cfg.e7_listen_host, "127.0.0.1") == 0);
    assert(cfg.e7_listen_port == 4334);
    assert(strcmp(cfg.e7_transport, "raw") == 0);
    assert(cfg.e7_lab_insecure_raw == 0);
    assert(strcmp(cfg.e7_reload_policy, "merge") == 0);
    assert(cfg.e7_auto_subscribe_unknown == 0);
    assert(cfg.e7_dirty_cap == 4096);
    assert(cfg.e7_rss_budget_bytes == 134217728u);
    assert(cfg.e7_max_sessions == 32);
    assert(cfg.e7_shelf_count == 2);
    assert(strcmp(cfg.e7_shelves[0].mac, "00:02:5d:d9:21:47") == 0);
    assert(strcmp(cfg.e7_shelves[0].shelf_id, "lab-e7-1") == 0);
    assert(cfg.e7_shelves[0].enabled == 1);
    assert(strcmp(cfg.e7_shelves[1].mac, "aa:bb:cc:dd:ee:ff") == 0);
    assert(strcmp(cfg.e7_shelves[1].shelf_id, "spare") == 0);
    assert(cfg.e7_shelves[1].enabled == 0);
    assert(edge_config_validate(&cfg, err, sizeof(err)) == 0);
    printf("  PASS: e7_callhome yaml parse + validate\n");
}

static void test_e7_raw_non_loopback_requires_lab_flag(void)
{
    edge_config_t c;
    char err[160];

    edge_config_defaults(&c);
    snprintf(c.e7_listen_host, sizeof(c.e7_listen_host), "0.0.0.0");
    snprintf(c.e7_transport, sizeof(c.e7_transport), "raw");
    c.e7_lab_insecure_raw = 0;
    assert(edge_config_validate(&c, err, sizeof(err)) == -1);
    assert(strstr(err, "lab_insecure_raw") != NULL);

    c.e7_lab_insecure_raw = 1;
    assert(edge_config_validate(&c, err, sizeof(err)) == 0);

    /* loopback raw ok without flag */
    edge_config_defaults(&c);
    snprintf(c.e7_listen_host, sizeof(c.e7_listen_host), "127.0.0.1");
    c.e7_lab_insecure_raw = 0;
    assert(edge_config_validate(&c, err, sizeof(err)) == 0);

    /* ssh on non-loopback does not require lab_insecure_raw */
    snprintf(c.e7_listen_host, sizeof(c.e7_listen_host), "0.0.0.0");
    snprintf(c.e7_transport, sizeof(c.e7_transport), "ssh");
    c.e7_lab_insecure_raw = 0;
    assert(edge_config_validate(&c, err, sizeof(err)) == 0);
    printf("  PASS: e7 raw non-loopback requires lab_insecure_raw\n");
}

static void test_load_e7_lab_path(void)
{
    static const char *candidates[] = {
        "config/edgehost.e7-lab.yaml",
        "../config/edgehost.e7-lab.yaml",
        NULL
    };
    const char *path = NULL;
    edge_config_t cfg;
    char err[160];
    size_t i;

    for (i = 0; candidates[i]; i++) {
        if (access(candidates[i], R_OK) == 0) {
            path = candidates[i];
            break;
        }
    }
    if (!path) {
        printf("  SKIP: e7-lab yaml not found from cwd\n");
        return;
    }
    assert(edge_config_load_yaml_path(path, &cfg, err, sizeof(err)) == 0);
    /* PR-4a: lab YAML enables Call Home listen */
    assert(cfg.e7_enabled == 1);
    assert(cfg.e7_listen_port == 4334);
    assert(cfg.e7_shelf_count >= 1);
    assert(cfg.e7_shelves[0].mac[0] != '\0');
    /* K10 / PR-2a: e7-lab enables net.pon + inventory with higher caps */
    assert(cfg.state_max_value_bytes == 2048);
    assert(cfg.state_max_keys_default == 1024);
    assert(cfg.state_net_pon_enabled == 1);
    assert(cfg.state_net_pon_max_keys == 16384);
    assert(cfg.state_inventory_enabled == 1);
    assert(cfg.state_inventory_max_keys == 512);
    assert(edge_config_validate(&cfg, err, sizeof(err)) == 0);
    printf("  PASS: load e7-lab path (%s)\n", path);
}

/** K10: state.max_keys_default / max_value_bytes / per-ns max_keys. */
static void test_state_capacity_yaml(void)
{
    const char *yaml =
        "state:\n"
        "  max_keys_default: 256\n"
        "  max_value_bytes: 2048\n"
        "  namespaces:\n"
        "    net_core:\n"
        "      enabled: true\n"
        "      max_keys: 128\n"
        "    map_dynamic:\n"
        "      enabled: true\n"
        "    net_pon:\n"
        "      enabled: true\n"
        "      max_keys: 16384\n"
        "    inventory:\n"
        "      enabled: true\n"
        "      max_keys: 512\n";
    edge_config_t cfg;
    char err[128];

    assert(edge_config_load_yaml_buf(yaml, strlen(yaml), &cfg, err,
                                     sizeof(err)) == 0);
    assert(cfg.state_max_keys_default == 256);
    assert(cfg.state_max_value_bytes == 2048);
    assert(cfg.state_net_core_max_keys == 128);
    assert(cfg.state_net_pon_enabled == 1);
    assert(cfg.state_net_pon_max_keys == 16384);
    assert(cfg.state_inventory_enabled == 1);
    assert(cfg.state_inventory_max_keys == 512);
    assert(edge_config_validate(&cfg, err, sizeof(err)) == 0);
    printf("  PASS: state capacity yaml parse\n");
}

int main(void)
{
    printf("edgecore_config:\n");
    test_defaults_validate();
    test_load_buf_and_apply();
    test_reject_keeps_previous();
    test_load_example_path();
    test_reload_and_hup();
    test_e7_callhome_yaml();
    test_e7_raw_non_loopback_requires_lab_flag();
    test_load_e7_lab_path();
    test_state_capacity_yaml();
    printf("all passed\n");
    return 0;
}
