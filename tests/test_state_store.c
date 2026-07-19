/**
 * P1.7a: state store unit + REST via edge_http1_serve.
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

int main(void)
{
    printf("state_store:\n");
    test_store_basic();
    test_rest_put_get_delete();
    test_rest_disabled_ns();
    printf("all passed\n");
    return 0;
}
