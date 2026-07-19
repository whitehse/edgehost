/**
 * P1.7b: WS accept key, STATE_CHANGED format, hub fan-out, HTTP upgrade.
 */
#include "edge_http1_serve.h"
#include "edge_metrics.h"
#include "edge_state.h"
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
    printf("All WS stream tests PASSED\n");
    return 0;
}
