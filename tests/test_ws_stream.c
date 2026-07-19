/**
 * P1.7b / PR-1: WS accept key, STATE_CHANGED format, hub fan-out, HTTP upgrade,
 * put_and_notify choke point.
 */
#include "edge_http1_serve.h"
#include "edge_metrics.h"
#include "edge_plugin_host.h"
#include "edge_state.h"
#include "edge_state_notify.h"
#include "edge_ws.h"

#include "websocket.h"
#include "protocol_events.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_accept_key_rfc6455(void)
{
    char accept[64];
    /* RFC 6455 example */
    assert(edge_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", accept,
                              sizeof(accept)) == 0);
    assert(strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
    printf("  PASS: RFC 6455 accept key\n");
}

static void test_format_state_changed(void)
{
    char buf[512];
    int n;

    n = edge_ws_format_state_changed(buf, sizeof(buf), "map.dynamic",
                                     "feature/fiber/1", "put",
                                     "{\"id\":\"1\",\"status\":\"down\"}", 26,
                                     "rid-1");
    assert(n > 0);
    assert(strstr(buf, "\"type\":\"STATE_CHANGED\"") != NULL);
    assert(strstr(buf, "\"ns\":\"map.dynamic\"") != NULL);
    assert(strstr(buf, "\"key\":\"feature/fiber/1\"") != NULL);
    assert(strstr(buf, "\"op\":\"put\"") != NULL);
    assert(strstr(buf, "\"id\":\"1\"") != NULL);
    assert(strstr(buf, "\"request_id\":\"rid-1\"") != NULL);

    n = edge_ws_format_state_changed(buf, sizeof(buf), "net.core", "router/r1",
                                     "delete", NULL, 0, "rid-2");
    assert(n > 0);
    assert(strstr(buf, "\"op\":\"delete\"") != NULL);
    assert(strstr(buf, "\"value\":null") != NULL);
    printf("  PASS: STATE_CHANGED format\n");
}

static void test_hub_fanout(void)
{
    edge_ws_hub_t *h = edge_ws_hub_create(4);
    char msg[EDGE_WS_MSG_MAX];
    size_t mlen = 0;
    int rc;

    assert(h);
    assert(edge_ws_hub_subscribe(h, 0) == 0);
    assert(edge_ws_hub_subscribe(h, 2) == 0);
    assert(edge_ws_hub_subscriber_count(h) == 2);

    assert(edge_ws_hub_broadcast_state_changed(
               h, "net.core", "router/r1", "put", "{\"x\":1}", 7, "abc") == 2);

    rc = edge_ws_hub_take_pending(h, 0, msg, sizeof(msg), &mlen);
    assert(rc == 1);
    assert(strstr(msg, "STATE_CHANGED") != NULL);
    assert(strstr(msg, "router/r1") != NULL);

    rc = edge_ws_hub_take_pending(h, 2, msg, sizeof(msg), &mlen);
    assert(rc == 1);
    assert(strstr(msg, "\"request_id\":\"abc\"") != NULL);

    rc = edge_ws_hub_take_pending(h, 1, msg, sizeof(msg), &mlen);
    assert(rc == 0); /* not subscribed */

    edge_ws_hub_unsubscribe(h, 0);
    assert(edge_ws_hub_subscriber_count(h) == 1);

    edge_ws_hub_destroy(h);
    printf("  PASS: hub fan-out\n");
}

static void test_http_upgrade_101(void)
{
    edge_http1_serve_t *s = edge_http1_serve_create();
    edge_metrics_t m;
    char resp[1024];
    size_t rlen = 0;
    int rc;
    const char *req =
        "GET /api/v1/stream?topics=state HTTP/1.1\r\n"
        "Host: t\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    edge_metrics_init(&m);
    assert(s);
    rc = edge_http1_serve_feed(s, (const uint8_t *)req, strlen(req), &m, resp,
                               sizeof(resp), &rlen);
    assert(rc == 1);
    assert(edge_http1_serve_took_ws_upgrade(s));
    assert(strstr(resp, "101 Switching Protocols") != NULL);
    assert(strstr(resp, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") !=
           NULL);
    assert(strstr(resp, "Upgrade: websocket") != NULL);

    edge_http1_serve_destroy(s);
    printf("  PASS: HTTP upgrade 101\n");
}

static void test_put_notifies_hub(void)
{
    edge_state_store_t *st = edge_state_create();
    edge_ws_hub_t *hub = edge_ws_hub_create(4);
    edge_http1_serve_t *s = edge_http1_serve_create();
    edge_metrics_t m;
    char resp[1024];
    char msg[EDGE_WS_MSG_MAX];
    size_t rlen = 0, mlen = 0;
    int rc;
    const char *put =
        "PUT /api/v1/state/net.core/router/r1 HTTP/1.1\r\n"
        "Host: t\r\n"
        "X-Request-Id: client-rid\r\n"
        "Content-Length: 25\r\n"
        "\r\n"
        "{\"id\":\"r1\",\"status\":\"ok\"}";

    edge_metrics_init(&m);
    assert(st && hub && s);
    edge_http1_serve_set_state(s, st);
    edge_http1_serve_set_ws_hub(s, hub);
    assert(edge_ws_hub_subscribe(hub, 0) == 0);

    rc = edge_http1_serve_feed(s, (const uint8_t *)put, strlen(put), &m, resp,
                               sizeof(resp), &rlen);
    assert(rc == 1);
    assert(strstr(resp, "204") != NULL);

    rc = edge_ws_hub_take_pending(hub, 0, msg, sizeof(msg), &mlen);
    assert(rc == 1);
    assert(strstr(msg, "STATE_CHANGED") != NULL);
    assert(strstr(msg, "\"op\":\"put\"") != NULL);
    assert(strstr(msg, "router/r1") != NULL);
    assert(strstr(msg, "client-rid") != NULL);
    assert(strstr(msg, "\"status\":\"ok\"") != NULL);

    /* DELETE */
    edge_http1_serve_reset(s);
    edge_http1_serve_set_state(s, st);
    edge_http1_serve_set_ws_hub(s, hub);
    {
        const char *del =
            "DELETE /api/v1/state/net.core/router/r1 HTTP/1.1\r\n"
            "Host: t\r\n"
            "\r\n";
        rlen = 0;
        rc = edge_http1_serve_feed(s, (const uint8_t *)del, strlen(del), &m,
                                   resp, sizeof(resp), &rlen);
        assert(rc == 1);
        assert(strstr(resp, "204") != NULL);
    }
    rc = edge_ws_hub_take_pending(hub, 0, msg, sizeof(msg), &mlen);
    assert(rc == 1);
    assert(strstr(msg, "\"op\":\"delete\"") != NULL);
    assert(strstr(msg, "\"value\":null") != NULL);

    edge_http1_serve_destroy(s);
    edge_ws_hub_destroy(hub);
    edge_state_destroy(st);
    printf("  PASS: PUT/DELETE notify hub\n");
}

static void test_ws_frame_roundtrip(void)
{
    /* Hub JSON → websocket_send_text → client feed → TEXT event */
    websocket_ctx_t *srv = websocket_create(WS_ROLE_SERVER);
    websocket_ctx_t *cli = websocket_create(WS_ROLE_CLIENT);
    char json[256];
    uint8_t frame[512];
    size_t flen;
    protocol_event_t ev;
    int n;

    n = edge_ws_format_state_changed(json, sizeof(json), "net.core", "k", "put",
                                     "{\"v\":1}", 7, "r");
    assert(n > 0);
    assert(srv && cli);
    assert(websocket_send_text(srv, json) == 0);
    flen = websocket_get_output(srv, frame, sizeof(frame));
    assert(flen > 0);
    (void)websocket_feed_input(cli, frame, flen);
    assert(websocket_next_event(cli, &ev) == 1);
    assert(ev.type == WS_EVENT_TEXT);
    assert(ev.u.ws_message.len == (size_t)n);
    assert(memcmp(ev.u.ws_message.data, json, (size_t)n) == 0);

    websocket_destroy(srv);
    websocket_destroy(cli);
    printf("  PASS: WS frame round-trip\n");
}

static void test_path_is_stream(void)
{
    assert(edge_ws_path_is_stream("/api/v1/stream"));
    assert(edge_ws_path_is_stream("/api/v1/stream?topics=state"));
    assert(!edge_ws_path_is_stream("/api/v1/state/net.core/x"));
    assert(!edge_ws_path_is_stream("/stream"));
    printf("  PASS: stream path match\n");
}

static void test_put_and_notify(void)
{
    edge_state_store_t *st = edge_state_create();
    edge_ws_hub_t *hub = edge_ws_hub_create(4);
    char msg[EDGE_WS_MSG_MAX];
    char buf[64];
    size_t mlen = 0, n = 0;
    edge_state_err_t er;

    assert(st && hub);
    assert(edge_ws_hub_subscribe(hub, 0) == 0);

    er = edge_state_put_and_notify(st, hub, "net.core", "router/r2",
                                   "{\"x\":2}", 7, "rid-put", 0);
    assert(er == EDGE_STATE_OK);
    assert(edge_state_get(st, "net.core", "router/r2", buf, sizeof(buf), &n) ==
           EDGE_STATE_OK);
    assert(n == 7);
    assert(edge_ws_hub_take_pending(hub, 0, msg, sizeof(msg), &mlen) == 1);
    assert(strstr(msg, "STATE_CHANGED") != NULL);
    assert(strstr(msg, "router/r2") != NULL);
    assert(strstr(msg, "\"op\":\"put\"") != NULL);
    assert(strstr(msg, "rid-put") != NULL);
    assert(strstr(msg, "\"x\":2") != NULL);

    /* hub NULL: put succeeds, no pending */
    er = edge_state_put_and_notify(st, NULL, "net.core", "router/r3",
                                   "{\"y\":3}", 7, NULL, 0);
    assert(er == EDGE_STATE_OK);
    assert(edge_ws_hub_take_pending(hub, 0, msg, sizeof(msg), &mlen) == 0);

    er = edge_state_delete_and_notify(st, hub, "net.core", "router/r2",
                                      "rid-del", 0);
    assert(er == EDGE_STATE_OK);
    assert(edge_ws_hub_take_pending(hub, 0, msg, sizeof(msg), &mlen) == 1);
    assert(strstr(msg, "\"op\":\"delete\"") != NULL);
    assert(strstr(msg, "rid-del") != NULL);

    /* failed put does not notify */
    er = edge_state_put_and_notify(st, hub, "net.core", "Bad Key!", "{}", 2,
                                   NULL, 0);
    assert(er != EDGE_STATE_OK);
    assert(edge_ws_hub_take_pending(hub, 0, msg, sizeof(msg), &mlen) == 0);

    edge_ws_hub_destroy(hub);
    edge_state_destroy(st);
    printf("  PASS: put_and_notify / delete_and_notify\n");
}

static void test_plugin_state_put_notifies(void)
{
    edge_state_store_t *st = edge_state_create();
    edge_ws_hub_t *hub = edge_ws_hub_create(4);
    edge_plugin_host_config_t cfg;
    edge_plugin_host_t *ph;
    const edge_host_api_t *api;
    char msg[EDGE_WS_MSG_MAX];
    size_t mlen = 0;

    assert(st && hub);
    memset(&cfg, 0, sizeof(cfg));
    cfg.state = st;
    ph = edge_plugin_host_create(&cfg);
    assert(ph);
    edge_plugin_host_set_ws_hub(ph, hub);
    assert(edge_plugin_host_ws_hub(ph) == hub);
    assert(edge_ws_hub_subscribe(hub, 1) == 0);

    api = edge_plugin_host_api(ph);
    assert(api && api->state_put);
    assert(api->state_put(api->ctx, "net.core", "from/plugin",
                          "{\"via\":\"plugin\"}", 16) == 0);
    assert(edge_ws_hub_take_pending(hub, 1, msg, sizeof(msg), &mlen) == 1);
    assert(strstr(msg, "STATE_CHANGED") != NULL);
    assert(strstr(msg, "from/plugin") != NULL);
    assert(strstr(msg, "plugin") != NULL);

    /* without hub: put ok, no message */
    edge_plugin_host_set_ws_hub(ph, NULL);
    assert(api->state_put(api->ctx, "net.core", "from/plugin2", "{}", 2) == 0);
    assert(edge_ws_hub_take_pending(hub, 1, msg, sizeof(msg), &mlen) == 0);

    edge_plugin_host_destroy(ph);
    edge_ws_hub_destroy(hub);
    edge_state_destroy(st);
    printf("  PASS: plugin host state_put → STATE_CHANGED\n");
}

static void test_format_fail_compact(void)
{
    edge_ws_hub_t *h = edge_ws_hub_create(2);
    char huge[EDGE_WS_MSG_MAX];
    char msg[EDGE_WS_MSG_MAX];
    size_t mlen = 0;
    size_t i;

    assert(h);
    assert(edge_ws_hub_subscribe(h, 0) == 0);
    /* value alone ≈ MSG_MAX so envelope cannot fit → format fail + compact */
    huge[0] = '{';
    for (i = 1; i < sizeof(huge) - 2; i++) {
        huge[i] = 'a';
    }
    huge[sizeof(huge) - 2] = '}';
    huge[sizeof(huge) - 1] = '\0';
    assert(edge_ws_hub_broadcast_state_changed(h, "net.core", "big/key", "put",
                                               huge, strlen(huge), "r") == 1);
    assert(edge_ws_hub_format_fail_count(h) >= 1);
    assert(edge_ws_hub_take_pending(h, 0, msg, sizeof(msg), &mlen) == 1);
    assert(strstr(msg, "truncated") != NULL || strstr(msg, "\"value\":null") != NULL);

    edge_ws_hub_destroy(h);
    printf("  PASS: format-fail compact fallback\n");
}

int main(void)
{
    printf("edgehost_ws_test:\n");
    test_accept_key_rfc6455();
    test_format_state_changed();
    test_path_is_stream();
    test_hub_fanout();
    test_http_upgrade_101();
    test_put_notifies_hub();
    test_ws_frame_roundtrip();
    test_put_and_notify();
    test_plugin_state_put_notifies();
    test_format_fail_compact();
    printf("All WS stream tests PASSED\n");
    return 0;
}
