/**
 * P1.8a: pending_table + plugin ABI PENDING path (mock outbound).
 */
#include "edge_pending.h"
#include "edge_plugin.h"
#include "edge_plugin_host.h"
#include "edge_state.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* --- test HTTP plugin: PENDING echo via mock upstream -------------------- */

typedef struct {
    int inited;
    int complete_calls;
    int force_pending_without_outbound;
    int force_double_pending_on_complete;
} test_plugin_data_t;

static int test_init(edge_plugin_t *self, const edge_host_api_t *host,
                     const void *cfg)
{
    test_plugin_data_t *d = (test_plugin_data_t *)self->user_data;
    (void)host;
    (void)cfg;
    d->inited = 1;
    return 0;
}

static void test_shutdown(edge_plugin_t *self)
{
    test_plugin_data_t *d = (test_plugin_data_t *)self->user_data;
    d->inited = 0;
}

static int test_on_http(edge_plugin_t *self, const edge_http_req_t *req,
                        edge_http_res_t *res)
{
    test_plugin_data_t *d = (test_plugin_data_t *)self->user_data;
    edge_http_client_req_t creq;
    uint64_t oid = 0;

    (void)res;
    if (d->force_pending_without_outbound) {
        return EDGE_PLUGIN_PENDING;
    }

    /* sync path: GET /v1/echo/sync → 200 immediately */
    if (req->path && strstr(req->path, "/sync")) {
        res->status = 200;
        snprintf(res->content_type, sizeof(res->content_type), "text/plain");
        (void)edge_http_res_set_body(res, "sync-ok", 7);
        return EDGE_PLUGIN_OK;
    }

    memset(&creq, 0, sizeof(creq));
    creq.method = "POST";
    creq.url = "https://upstream.example/v1/echo";
    creq.body = req->body;
    creq.body_len = req->body_len;
    creq.timeout_ms = 5000;
    creq.user_tag = 0xABCDu;

    if (self->host->http_client_request(self->host->ctx, &creq, &oid) != 0) {
        res->status = 429;
        (void)edge_http_res_set_body(res, "{\"error\":\"PENDING_FULL\"}", 24);
        return EDGE_PLUGIN_ERR;
    }
    return EDGE_PLUGIN_PENDING;
}

static int test_on_http_complete(edge_plugin_t *self,
                                 const edge_http_client_result_t *up,
                                 edge_http_res_t *res)
{
    test_plugin_data_t *d = (test_plugin_data_t *)self->user_data;
    d->complete_calls++;

    if (d->force_double_pending_on_complete) {
        return EDGE_PLUGIN_PENDING;
    }
    if (up->transport_err == ETIMEDOUT) {
        res->status = 504;
        snprintf(res->reason, sizeof(res->reason), "Gateway Timeout");
        (void)edge_http_res_set_body(res, "{\"error\":\"UPSTREAM_TIMEOUT\"}", 28);
        return EDGE_PLUGIN_ERR;
    }
    if (up->transport_err != 0) {
        res->status = 502;
        (void)edge_http_res_set_body(res, "{\"error\":\"UPSTREAM_TRANSPORT\"}", 29);
        return EDGE_PLUGIN_ERR;
    }
    res->status = up->status ? up->status : 200;
    snprintf(res->content_type, sizeof(res->content_type), "application/json");
    if (up->body && up->body_len) {
        (void)edge_http_res_set_body(res, up->body, up->body_len);
    } else {
        (void)edge_http_res_set_body(res, "{}", 2);
    }
    return EDGE_PLUGIN_OK;
}

static const edge_plugin_vtbl_t test_vtbl = {
    .name = "test_echo",
    .version = "0.1",
    .kind = EDGE_PLUGIN_KIND_HTTP,
    .init = test_init,
    .shutdown = test_shutdown,
    .on_config_reload = NULL,
    .on_http = test_on_http,
    .on_http_complete = test_on_http_complete,
    .feed = NULL,
    .next_event = NULL,
    .on_tick = NULL,
};

static void make_res(edge_http_res_t *res, uint8_t *buf, size_t cap)
{
    memset(res, 0, sizeof(*res));
    res->body = buf;
    res->body_cap = cap;
    res->body_len = 0;
}

static void test_pending_table_basic(void)
{
    edge_pending_table_t *t = edge_pending_create(4);
    edge_plugin_t plug;
    edge_pending_entry_t *e;

    memset(&plug, 0, sizeof(plug));
    assert(t);
    assert(edge_pending_capacity(t) == 4);
    assert(edge_pending_count(t) == 0);

    assert(edge_pending_insert(t, 1, 100, &plug, 1, 1000, 0, "alice") == 0);
    assert(edge_pending_count(t) == 1);
    /* nested same inbound */
    assert(edge_pending_insert(t, 1, 101, &plug, 0, 0, 0, NULL) != 0);
    assert(edge_pending_insert(t, 2, 102, &plug, 0, 0, 0, NULL) == 0);
    assert(edge_pending_insert(t, 3, 103, &plug, 0, 0, 0, NULL) == 0);
    assert(edge_pending_insert(t, 4, 104, &plug, 0, 0, 0, NULL) == 0);
    assert(edge_pending_full(t));
    assert(edge_pending_insert(t, 5, 105, &plug, 0, 0, 0, NULL) != 0);

    e = edge_pending_find_outbound(t, 100);
    assert(e && e->inbound_slot == 1);
    assert(strcmp(e->principal_sub, "alice") == 0);
    assert(edge_pending_release_outbound(t, 100) == 0);
    assert(edge_pending_count(t) == 3);
    assert(!edge_pending_full(t));

    assert(edge_pending_release_inbound(t, 2) == 0);
    assert(edge_pending_find_inbound(t, 2) == NULL);

    edge_pending_destroy(t);
    printf("  PASS: pending_table basic\n");
}

static void test_sync_ok(void)
{
    edge_plugin_host_config_t cfg = {0};
    edge_plugin_host_t *h;
    test_plugin_data_t data;
    edge_plugin_t plugin;
    edge_http_req_t req;
    edge_http_res_t res;
    uint8_t body[256];
    int st;

    memset(&data, 0, sizeof(data));
    memset(&plugin, 0, sizeof(plugin));
    plugin.vtbl = &test_vtbl;
    plugin.user_data = &data;

    cfg.max_pending = 8;
    h = edge_plugin_host_create(&cfg);
    assert(h);
    assert(edge_plugin_host_register(h, &plugin, NULL) == 0);
    assert(data.inited);
    assert(edge_plugin_host_add_route(h, "/v1/echo", &plugin) == 0);
    assert(edge_plugin_host_match(h, "/v1/echo/sync") == &plugin);

    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.path = "/v1/echo/sync";
    req.inbound_slot = 3;
    make_res(&res, body, sizeof(body));

    st = edge_plugin_host_dispatch_http(h, &plugin, &req, &res);
    assert(st == EDGE_PLUGIN_OK);
    assert(res.status == 200);
    assert(res.body_len == 7);
    assert(memcmp(res.body, "sync-ok", 7) == 0);
    assert(edge_pending_count(edge_plugin_host_pending(h)) == 0);

    edge_plugin_host_destroy(h);
    printf("  PASS: sync OK path\n");
}

static void test_pending_complete_ok(void)
{
    edge_plugin_host_config_t cfg = {0};
    edge_plugin_host_t *h;
    test_plugin_data_t data;
    edge_plugin_t plugin;
    edge_http_req_t req;
    edge_http_res_t res;
    uint8_t body[512];
    int st;
    uint64_t oid;
    edge_http_client_result_t up;
    const edge_http_client_req_t *creq;
    static const uint8_t in_body[] = "{\"msg\":\"hi\"}";
    static const uint8_t up_body[] = "{\"echo\":\"hi\"}";

    memset(&data, 0, sizeof(data));
    memset(&plugin, 0, sizeof(plugin));
    plugin.vtbl = &test_vtbl;
    plugin.user_data = &data;

    cfg.max_pending = 8;
    h = edge_plugin_host_create(&cfg);
    assert(h);
    assert(edge_plugin_host_register(h, &plugin, NULL) == 0);

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.path = "/v1/echo";
    req.body = in_body;
    req.body_len = sizeof(in_body) - 1;
    req.inbound_slot = 7;
    make_res(&res, body, sizeof(body));

    st = edge_plugin_host_dispatch_http(h, &plugin, &req, &res);
    assert(st == EDGE_PLUGIN_PENDING);
    assert(edge_pending_count(edge_plugin_host_pending(h)) == 1);
    oid = edge_plugin_host_last_outbound_id(h);
    assert(oid != 0);
    creq = edge_plugin_host_last_client_req(h);
    assert(creq);
    assert(strcmp(creq->method, "POST") == 0);
    assert(strstr(creq->url, "upstream.example") != NULL);
    assert(creq->body_len == sizeof(in_body) - 1);
    assert(creq->user_tag == 0xABCDu);

    /* simulate upstream success */
    memset(&up, 0, sizeof(up));
    up.outbound_id = oid;
    up.user_tag = 0xABCDu;
    up.transport_err = 0;
    up.status = 200;
    up.body = up_body;
    up.body_len = sizeof(up_body) - 1;

    make_res(&res, body, sizeof(body));
    st = edge_plugin_host_complete_outbound(h, &up, &res);
    assert(st == EDGE_PLUGIN_OK);
    assert(data.complete_calls == 1);
    assert(res.status == 200);
    assert(res.body_len == sizeof(up_body) - 1);
    assert(memcmp(res.body, up_body, res.body_len) == 0);
    assert(edge_pending_count(edge_plugin_host_pending(h)) == 0);

    edge_plugin_host_destroy(h);
    printf("  PASS: PENDING → complete OK\n");
}

static void test_pending_timeout(void)
{
    edge_plugin_host_config_t cfg = {0};
    edge_plugin_host_t *h;
    test_plugin_data_t data;
    edge_plugin_t plugin;
    edge_http_req_t req;
    edge_http_res_t res;
    uint8_t body[256];
    edge_http_client_result_t up;
    int st;

    memset(&data, 0, sizeof(data));
    memset(&plugin, 0, sizeof(plugin));
    plugin.vtbl = &test_vtbl;
    plugin.user_data = &data;
    cfg.max_pending = 4;
    h = edge_plugin_host_create(&cfg);
    assert(edge_plugin_host_register(h, &plugin, NULL) == 0);

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.path = "/v1/echo";
    req.inbound_slot = 1;
    make_res(&res, body, sizeof(body));
    assert(edge_plugin_host_dispatch_http(h, &plugin, &req, &res) ==
           EDGE_PLUGIN_PENDING);

    memset(&up, 0, sizeof(up));
    up.outbound_id = edge_plugin_host_last_outbound_id(h);
    up.transport_err = ETIMEDOUT;
    make_res(&res, body, sizeof(body));
    st = edge_plugin_host_complete_outbound(h, &up, &res);
    assert(st == EDGE_PLUGIN_ERR);
    assert(res.status == 504);
    assert(strstr((char *)res.body, "TIMEOUT") != NULL);

    edge_plugin_host_destroy(h);
    printf("  PASS: upstream timeout → 504\n");
}

static void test_pending_without_outbound(void)
{
    edge_plugin_host_config_t cfg = {0};
    edge_plugin_host_t *h;
    test_plugin_data_t data;
    edge_plugin_t plugin;
    edge_http_req_t req;
    edge_http_res_t res;
    uint8_t body[256];

    memset(&data, 0, sizeof(data));
    data.force_pending_without_outbound = 1;
    memset(&plugin, 0, sizeof(plugin));
    plugin.vtbl = &test_vtbl;
    plugin.user_data = &data;
    h = edge_plugin_host_create(&cfg);
    assert(edge_plugin_host_register(h, &plugin, NULL) == 0);

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.path = "/v1/echo";
    req.inbound_slot = 2;
    make_res(&res, body, sizeof(body));
    assert(edge_plugin_host_dispatch_http(h, &plugin, &req, &res) ==
           EDGE_PLUGIN_ERR);
    assert(res.status == 500);
    assert(edge_pending_count(edge_plugin_host_pending(h)) == 0);

    edge_plugin_host_destroy(h);
    printf("  PASS: PENDING without outbound → 500\n");
}

static void test_table_full_429(void)
{
    edge_plugin_host_config_t cfg = {0};
    edge_plugin_host_t *h;
    test_plugin_data_t data;
    edge_plugin_t plugin;
    edge_http_req_t req;
    edge_http_res_t res;
    uint8_t body[256];
    int i;

    memset(&data, 0, sizeof(data));
    memset(&plugin, 0, sizeof(plugin));
    plugin.vtbl = &test_vtbl;
    plugin.user_data = &data;
    cfg.max_pending = 2;
    h = edge_plugin_host_create(&cfg);
    assert(edge_plugin_host_register(h, &plugin, NULL) == 0);

    for (i = 0; i < 2; i++) {
        memset(&req, 0, sizeof(req));
        req.method = "POST";
        req.path = "/v1/echo";
        req.inbound_slot = (uint32_t)(10 + i);
        make_res(&res, body, sizeof(body));
        assert(edge_plugin_host_dispatch_http(h, &plugin, &req, &res) ==
               EDGE_PLUGIN_PENDING);
    }
    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.path = "/v1/echo";
    req.inbound_slot = 99;
    make_res(&res, body, sizeof(body));
    assert(edge_plugin_host_dispatch_http(h, &plugin, &req, &res) ==
           EDGE_PLUGIN_ERR);
    assert(res.status == 429);

    edge_plugin_host_destroy(h);
    printf("  PASS: table full → 429\n");
}

static void test_cancel_inbound(void)
{
    edge_plugin_host_config_t cfg = {0};
    edge_plugin_host_t *h;
    test_plugin_data_t data;
    edge_plugin_t plugin;
    edge_http_req_t req;
    edge_http_res_t res;
    uint8_t body[128];

    memset(&data, 0, sizeof(data));
    memset(&plugin, 0, sizeof(plugin));
    plugin.vtbl = &test_vtbl;
    plugin.user_data = &data;
    h = edge_plugin_host_create(&cfg);
    assert(edge_plugin_host_register(h, &plugin, NULL) == 0);

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.path = "/v1/echo";
    req.inbound_slot = 42;
    make_res(&res, body, sizeof(body));
    assert(edge_plugin_host_dispatch_http(h, &plugin, &req, &res) ==
           EDGE_PLUGIN_PENDING);
    assert(edge_plugin_host_cancel_inbound(h, 42) == 0);
    assert(edge_pending_count(edge_plugin_host_pending(h)) == 0);
    assert(data.complete_calls == 0);

    edge_plugin_host_destroy(h);
    printf("  PASS: cancel inbound (no complete)\n");
}

static void test_state_via_host_api(void)
{
    edge_state_store_t *st = edge_state_create();
    edge_plugin_host_config_t cfg = {0};
    edge_plugin_host_t *h;
    char buf[64];
    size_t n = sizeof(buf);

    cfg.state = st;
    h = edge_plugin_host_create(&cfg);
    assert(h && st);
    {
        const edge_host_api_t *api = edge_plugin_host_api(h);
        assert(api);
        assert(api->state_put(api->ctx, "net.core", "k1", "{\"a\":1}", 7) == 0);
        n = sizeof(buf);
        assert(api->state_get(api->ctx, "net.core", "k1", buf, &n) == 0);
        assert(n == 7);
        assert(memcmp(buf, "{\"a\":1}", 7) == 0);
    }
    edge_plugin_host_destroy(h);
    edge_state_destroy(st);
    printf("  PASS: host API state_get/put\n");
}

static void test_double_pending_on_complete(void)
{
    edge_plugin_host_config_t cfg = {0};
    edge_plugin_host_t *h;
    test_plugin_data_t data;
    edge_plugin_t plugin;
    edge_http_req_t req;
    edge_http_res_t res;
    uint8_t body[256];
    edge_http_client_result_t up;

    memset(&data, 0, sizeof(data));
    data.force_double_pending_on_complete = 1;
    memset(&plugin, 0, sizeof(plugin));
    plugin.vtbl = &test_vtbl;
    plugin.user_data = &data;
    h = edge_plugin_host_create(&cfg);
    assert(edge_plugin_host_register(h, &plugin, NULL) == 0);

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.path = "/v1/echo";
    req.inbound_slot = 5;
    make_res(&res, body, sizeof(body));
    assert(edge_plugin_host_dispatch_http(h, &plugin, &req, &res) ==
           EDGE_PLUGIN_PENDING);

    memset(&up, 0, sizeof(up));
    up.outbound_id = edge_plugin_host_last_outbound_id(h);
    up.transport_err = 0;
    up.status = 200;
    make_res(&res, body, sizeof(body));
    assert(edge_plugin_host_complete_outbound(h, &up, &res) == EDGE_PLUGIN_ERR);
    assert(res.status == 500);
    assert(edge_pending_count(edge_plugin_host_pending(h)) == 0);

    edge_plugin_host_destroy(h);
    printf("  PASS: on_http_complete PENDING coerced to ERR\n");
}

int main(void)
{
    printf("edgehost_pending_plugin_test:\n");
    test_pending_table_basic();
    test_sync_ok();
    test_pending_complete_ok();
    test_pending_timeout();
    test_pending_without_outbound();
    test_table_full_429();
    test_cancel_inbound();
    test_state_via_host_api();
    test_double_pending_on_complete();
    printf("All pending/plugin tests PASSED\n");
    return 0;
}
