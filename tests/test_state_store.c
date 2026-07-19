/**
 * P1.7a / K10 PR-2a: state store unit + REST via edge_http1_serve.
 */
#include "edge_http1_serve.h"
#include "edge_metrics.h"
#include "edge_state.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_store_basic(void)
{
    edge_state_store_t *st = edge_state_create();
    char buf[256];
    size_t n = 0;
    edge_state_err_t e;
    char keys[8][EDGE_STATE_KEY_MAX];
    int nk;

    assert(st);
    assert(edge_state_ns_enabled(st, "net.core"));
    assert(edge_state_ns_enabled(st, "map.dynamic"));
    assert(!edge_state_ns_enabled(st, "net.pon"));

    e = edge_state_put(st, "net.core", "router/r1",
                       "{\"id\":\"r1\",\"status\":\"ok\"}", 25);
    assert(e == EDGE_STATE_OK);
    e = edge_state_get(st, "net.core", "router/r1", buf, sizeof(buf), &n);
    assert(e == EDGE_STATE_OK);
    assert(n == 25);
    assert(strstr(buf, "r1") != NULL);

    e = edge_state_put(st, "net.pon", "x", "{}", 2);
    assert(e == EDGE_STATE_NS_DISABLED);

    e = edge_state_put(st, "net.core", "Bad Key!", "{}", 2);
    assert(e == EDGE_STATE_BAD_KEY);

    e = edge_state_put(st, "map.dynamic", "feature/fiber/1",
                       "{\"id\":\"1\"}", 10);
    assert(e == EDGE_STATE_OK);

    nk = edge_state_list(st, "net.core", "router/", keys, 8);
    assert(nk == 1);
    assert(strcmp(keys[0], "router/r1") == 0);

    e = edge_state_delete(st, "net.core", "router/r1");
    assert(e == EDGE_STATE_OK);
    e = edge_state_get(st, "net.core", "router/r1", buf, sizeof(buf), &n);
    assert(e == EDGE_STATE_NOT_FOUND);

    edge_state_destroy(st);
    printf("  PASS: store basic\n");
}

static int feed_req(edge_http1_serve_t *s, const char *req, char *resp,
                    size_t cap, size_t *rlen, edge_metrics_t *m)
{
    return edge_http1_serve_feed(s, (const uint8_t *)req, strlen(req), m, resp,
                                 cap, rlen);
}

static void test_rest_put_get_delete(void)
{
    edge_state_store_t *st = edge_state_create();
    edge_http1_serve_t *s = edge_http1_serve_create();
    edge_metrics_t m;
    char resp[4096];
    size_t rlen = 0;
    int rc;
    const char *put =
        "PUT /api/v1/state/net.core/router/r1 HTTP/1.1\r\n"
        "Host: t\r\n"
        "Content-Length: 25\r\n"
        "\r\n"
        "{\"id\":\"r1\",\"status\":\"ok\"}";
    const char *get =
        "GET /api/v1/state/net.core/router/r1 HTTP/1.1\r\n"
        "Host: t\r\n"
        "\r\n";
    const char *list =
        "GET /api/v1/state/net.core?prefix=router/ HTTP/1.1\r\n"
        "Host: t\r\n"
        "\r\n";
    const char *del =
        "DELETE /api/v1/state/net.core/router/r1 HTTP/1.1\r\n"
        "Host: t\r\n"
        "\r\n";

    edge_metrics_init(&m);
    assert(st && s);
    edge_http1_serve_set_state(s, st);

    rc = feed_req(s, put, resp, sizeof(resp), &rlen, &m);
    assert(rc == 1);
    assert(strstr(resp, "204") != NULL || strstr(resp, "200") != NULL);

    edge_http1_serve_reset(s);
    edge_http1_serve_set_state(s, st);
    rc = feed_req(s, get, resp, sizeof(resp), &rlen, &m);
    assert(rc == 1);
    assert(strstr(resp, "200") != NULL);
    assert(strstr(resp, "\"id\":\"r1\"") != NULL);

    edge_http1_serve_reset(s);
    edge_http1_serve_set_state(s, st);
    rc = feed_req(s, list, resp, sizeof(resp), &rlen, &m);
    assert(rc == 1);
    assert(strstr(resp, "router/r1") != NULL);

    edge_http1_serve_reset(s);
    edge_http1_serve_set_state(s, st);
    rc = feed_req(s, del, resp, sizeof(resp), &rlen, &m);
    assert(rc == 1);
    assert(strstr(resp, "204") != NULL);

    edge_http1_serve_reset(s);
    edge_http1_serve_set_state(s, st);
    rc = feed_req(s, get, resp, sizeof(resp), &rlen, &m);
    assert(rc == 1);
    assert(strstr(resp, "404") != NULL);

    edge_http1_serve_destroy(s);
    edge_state_destroy(st);
    printf("  PASS: REST put/get/list/delete\n");
}

static void test_rest_disabled_ns(void)
{
    edge_state_store_t *st = edge_state_create();
    edge_http1_serve_t *s = edge_http1_serve_create();
    edge_metrics_t m;
    char resp[1024];
    size_t rlen = 0;
    const char *put =
        "PUT /api/v1/state/net.pon/x HTTP/1.1\r\n"
        "Host: t\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "{}";

    edge_metrics_init(&m);
    edge_http1_serve_set_state(s, st);
    assert(feed_req(s, put, resp, sizeof(resp), &rlen, &m) == 1);
    assert(strstr(resp, "403") != NULL || strstr(resp, "NS_DISABLED") != NULL);

    edge_http1_serve_destroy(s);
    edge_state_destroy(st);
    printf("  PASS: disabled ns\n");
}

static void test_apply_config_enable_hooks(void)
{
    edge_state_store_t *st = edge_state_create();
    edge_config_t cfg;
    edge_state_err_t e;

    assert(st);
    edge_config_defaults(&cfg);
    assert(!edge_state_ns_enabled(st, "net.pon"));
    assert(!edge_state_ns_enabled(st, "net.home"));
    assert(!edge_state_ns_enabled(st, "electric"));
    assert(!edge_state_ns_enabled(st, "inventory"));

    cfg.state_net_pon_enabled = 1;
    cfg.state_net_home_enabled = 1;
    cfg.state_electric_enabled = 1;
    cfg.state_inventory_enabled = 1;
    edge_state_apply_config(st, &cfg);

    assert(edge_state_ns_enabled(st, "net.pon"));
    assert(edge_state_ns_enabled(st, "net.home"));
    assert(edge_state_ns_enabled(st, "electric"));
    assert(edge_state_ns_enabled(st, "inventory"));

    e = edge_state_put(st, "net.pon", "olt/olt-1",
                       "{\"id\":\"olt-1\",\"status\":\"ok\"}", 30);
    assert(e == EDGE_STATE_OK);
    e = edge_state_put(st, "inventory", "asset/a-1", "{\"id\":\"a-1\"}", 12);
    assert(e == EDGE_STATE_OK);

    /* ext.* via dynamic register */
    assert(edge_state_ns_set_enabled(st, "ext.vendor", 1) == 0);
    assert(edge_state_ns_enabled(st, "ext.vendor"));
    e = edge_state_put(st, "ext.vendor", "probe/1", "{}", 2);
    assert(e == EDGE_STATE_OK);

    /* disable again */
    cfg.state_net_pon_enabled = 0;
    edge_state_apply_config(st, &cfg);
    assert(!edge_state_ns_enabled(st, "net.pon"));
    e = edge_state_put(st, "net.pon", "x", "{}", 2);
    assert(e == EDGE_STATE_NS_DISABLED);

    edge_state_destroy(st);
    printf("  PASS: apply_config enable hooks + ext.*\n");
}

/** K10: disabled ns has no entry table (capacity 0 / stub only). */
static void test_disabled_ns_no_table(void)
{
    edge_state_store_t *st = edge_state_create();

    assert(st);
    assert(!edge_state_ns_enabled(st, "net.pon"));
    assert(edge_state_ns_capacity(st, "net.pon") == 0);
    assert(edge_state_ns_capacity(st, "net.home") == 0);
    assert(edge_state_ns_capacity(st, "electric") == 0);
    assert(edge_state_ns_capacity(st, "inventory") == 0);
    /* Default-enabled ns have tables */
    assert(edge_state_ns_capacity(st, "net.core") == EDGE_STATE_KEYS_DEFAULT);
    assert(edge_state_ns_capacity(st, "map.dynamic") == EDGE_STATE_KEYS_DEFAULT);

    edge_state_destroy(st);
    printf("  PASS: disabled ns no entry table\n");
}

/** K10: enable with max_keys=100 → 100 puts ok, 101st NS_FULL. */
static void test_per_ns_capacity_full(void)
{
    edge_state_store_t *st = edge_state_create();
    edge_state_err_t e;
    char key[64];
    int i;

    assert(st);
    assert(edge_state_ns_set_capacity(st, "net.pon", 100) == 0);
    assert(edge_state_ns_set_enabled(st, "net.pon", 1) == 0);
    assert(edge_state_ns_enabled(st, "net.pon"));
    assert(edge_state_ns_capacity(st, "net.pon") == 100);
    assert(edge_state_ns_max_keys(st, "net.pon") == 100);

    for (i = 0; i < 100; i++) {
        snprintf(key, sizeof(key), "k/%d", i);
        e = edge_state_put(st, "net.pon", key, "{}", 2);
        assert(e == EDGE_STATE_OK);
    }
    e = edge_state_put(st, "net.pon", "k/overflow", "{}", 2);
    assert(e == EDGE_STATE_NS_FULL);
    assert(edge_state_count(st, "net.pon") == 100);

    edge_state_destroy(st);
    printf("  PASS: per-ns capacity full at max_keys\n");
}

/** K10: different namespaces may have different capacities. */
static void test_different_ns_capacities(void)
{
    edge_state_store_t *st = edge_state_create();
    edge_config_t cfg;
    edge_state_err_t e;
    char key[64];
    int i;

    assert(st);
    edge_config_defaults(&cfg);
    cfg.state_net_pon_enabled = 1;
    cfg.state_inventory_enabled = 1;
    cfg.state_net_pon_max_keys = 50;
    cfg.state_inventory_max_keys = 10;
    edge_state_apply_config(st, &cfg);

    assert(edge_state_ns_capacity(st, "net.pon") == 50);
    assert(edge_state_ns_capacity(st, "inventory") == 10);
    assert(edge_state_ns_capacity(st, "net.core") == EDGE_STATE_KEYS_DEFAULT);

    for (i = 0; i < 10; i++) {
        snprintf(key, sizeof(key), "a/%d", i);
        e = edge_state_put(st, "inventory", key, "{}", 2);
        assert(e == EDGE_STATE_OK);
    }
    e = edge_state_put(st, "inventory", "a/x", "{}", 2);
    assert(e == EDGE_STATE_NS_FULL);

    for (i = 0; i < 50; i++) {
        snprintf(key, sizeof(key), "p/%d", i);
        e = edge_state_put(st, "net.pon", key, "{}", 2);
        assert(e == EDGE_STATE_OK);
    }
    e = edge_state_put(st, "net.pon", "p/x", "{}", 2);
    assert(e == EDGE_STATE_NS_FULL);

    edge_state_destroy(st);
    printf("  PASS: different ns capacities\n");
}

/**
 * K10 / ADR-007: put path still uses pre-allocated value buffers.
 * Fill all keys in a tiny ns; overwrite existing key still works (no malloc).
 */
static void test_no_put_path_malloc_overwrite(void)
{
    edge_state_config_t sc = edge_state_default_config();
    edge_state_store_t *st;
    edge_state_err_t e;
    char buf[128];
    size_t n = 0;
    const char *v1 = "{\"n\":1}";
    const char *v2 = "{\"n\":2,\"extra\":true}";

    sc.max_keys_per_ns = 4;
    sc.max_value_bytes = 64;
    st = edge_state_create_with_config(&sc);
    assert(st);

    e = edge_state_put(st, "net.core", "a", v1, strlen(v1));
    assert(e == EDGE_STATE_OK);
    e = edge_state_put(st, "net.core", "a", v2, strlen(v2));
    assert(e == EDGE_STATE_OK);
    e = edge_state_get(st, "net.core", "a", buf, sizeof(buf), &n);
    assert(e == EDGE_STATE_OK);
    assert(n == strlen(v2));
    assert(strcmp(buf, v2) == 0);

    /* Free on disable reclaims table */
    assert(edge_state_ns_set_enabled(st, "net.core", 0) == 0);
    assert(edge_state_ns_capacity(st, "net.core") == 0);
    e = edge_state_put(st, "net.core", "a", v1, strlen(v1));
    assert(e == EDGE_STATE_NS_DISABLED);

    edge_state_destroy(st);
    printf("  PASS: put overwrite + free on disable\n");
}

/** YAML-shaped apply: max_value_bytes via create + per-ns max_keys. */
static void test_config_from_edge_config(void)
{
    edge_config_t cfg;
    edge_state_config_t sc;
    edge_state_store_t *st;
    edge_state_err_t e;

    edge_config_defaults(&cfg);
    cfg.state_max_keys_default = 32;
    cfg.state_max_value_bytes = 2048;
    cfg.state_net_pon_enabled = 1;
    cfg.state_net_pon_max_keys = 16;
    cfg.state_inventory_enabled = 1;
    cfg.state_inventory_max_keys = 8;

    sc = edge_state_config_from_edge_config(&cfg);
    assert(sc.max_keys_per_ns == 32);
    assert(sc.max_value_bytes == 2048);

    st = edge_state_create_with_config(&sc);
    assert(st);
    edge_state_apply_config(st, &cfg);

    assert(edge_state_ns_capacity(st, "net.core") == 32);
    assert(edge_state_ns_capacity(st, "net.pon") == 16);
    assert(edge_state_ns_capacity(st, "inventory") == 8);

    e = edge_state_put(st, "net.pon", "x", "{\"ok\":1}", 8);
    assert(e == EDGE_STATE_OK);
    /* value larger than 2048 rejected */
    {
        char big[2100];
        memset(big, 'x', sizeof(big));
        big[0] = '"';
        big[2098] = '"';
        big[2099] = '\0';
        e = edge_state_put(st, "net.pon", "big", big, 2099);
        assert(e == EDGE_STATE_TOO_LARGE);
    }

    edge_state_destroy(st);
    printf("  PASS: config_from_edge_config + compact max_value\n");
}

int main(void)
{
    printf("state_store:\n");
    test_store_basic();
    test_rest_put_get_delete();
    test_rest_disabled_ns();
    test_apply_config_enable_hooks();
    test_disabled_ns_no_table();
    test_per_ns_capacity_full();
    test_different_ns_capacities();
    test_no_put_path_malloc_overwrite();
    test_config_from_edge_config();
    printf("all passed\n");
    return 0;
}
