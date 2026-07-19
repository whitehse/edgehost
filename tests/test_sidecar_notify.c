/**
 * P1.11 pq_sidecar + P1.12 NOTIFY apply tests.
 */
#include "edge_notify.h"
#include "edge_pq_sidecar.h"
#include "edge_state.h"
#include "edge_ws.h"

#include "edge_outbound.h"
#include "edge_plugin.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *sample_prom =
    "# HELP pqproxy_live_backends Live backend connections\n"
    "# TYPE pqproxy_live_backends gauge\n"
    "pqproxy_live_backends 4\n"
    "# TYPE pqproxy_active_frontends gauge\n"
    "pqproxy_active_frontends 2\n"
    "# TYPE pqproxy_pool_waiters gauge\n"
    "pqproxy_pool_waiters 1\n";

static void test_parse_metrics(void)
{
    char health[512];
    int up = 0;
    int64_t busy = 0, live = 0, active = 0;

    assert(edge_pq_sidecar_parse_metrics(sample_prom, strlen(sample_prom),
                                         health, sizeof(health), &up, &busy,
                                         &live, &active) == 0);
    assert(up == 1);
    assert(busy == 1);
    assert(live == 4);
    assert(active == 2);
    assert(strstr(health, "\"up\":true") != NULL);
    assert(strstr(health, "\"pool_busy\":1") != NULL);
    printf("  PASS: parse prometheus metrics\n");
}

typedef struct {
    int port;
    int ready;
    int stop;
} mock_t;

static void *metrics_thread(void *arg)
{
    mock_t *m = (mock_t *)arg;
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    struct sockaddr_in addr;

    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(lfd, (struct sockaddr *)&addr, sizeof(addr));
    {
        socklen_t al = sizeof(addr);
        getsockname(lfd, (struct sockaddr *)&addr, &al);
        m->port = ntohs(addr.sin_port);
    }
    listen(lfd, 4);
    m->ready = 1;
    while (!m->stop) {
        fd_set rf;
        struct timeval tv = {0, 50000};
        FD_ZERO(&rf);
        FD_SET(lfd, &rf);
        if (select(lfd + 1, &rf, NULL, NULL, &tv) <= 0) {
            continue;
        }
        int c = accept(lfd, NULL, NULL);
        if (c < 0) {
            continue;
        }
        char req[512];
        (void)read(c, req, sizeof(req));
        char resp[2048];
        int n = snprintf(resp, sizeof(resp),
                         "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                         "Connection: close\r\n\r\n%s",
                         strlen(sample_prom), sample_prom);
        (void)write(c, resp, (size_t)n);
        close(c);
    }
    close(lfd);
    return NULL;
}

static void test_scrape_once(void)
{
    mock_t m;
    pthread_t th;
    edge_pq_sidecar_config_t cfg;
    edge_state_store_t *st;
    edge_ws_hub_t *hub;
    char buf[512];
    size_t n = 0;
    char msg[EDGE_WS_MSG_MAX];
    size_t mlen = 0;

    memset(&m, 0, sizeof(m));
    assert(pthread_create(&th, NULL, metrics_thread, &m) == 0);
    while (!m.ready) {
        usleep(1000);
    }

    edge_pq_sidecar_config_defaults(&cfg);
    cfg.enabled = 1;
    snprintf(cfg.metrics_url, sizeof(cfg.metrics_url),
             "http://127.0.0.1:%d/metrics", m.port);
    st = edge_state_create();
    hub = edge_ws_hub_create(4);
    assert(st && hub);
    assert(edge_ws_hub_subscribe(hub, 0) == 0);
    assert(edge_pq_sidecar_scrape_once(&cfg, st, hub) == 0);
    assert(edge_state_get(st, "net.core", "pqproxy/health", buf, sizeof(buf),
                          &n) == EDGE_STATE_OK);
    assert(strstr(buf, "\"up\":true") != NULL);
    assert(edge_ws_hub_take_pending(hub, 0, msg, sizeof(msg), &mlen) == 1);
    assert(strstr(msg, "STATE_CHANGED") != NULL);
    assert(strstr(msg, "pqproxy/health") != NULL);

    m.stop = 1;
    pthread_join(th, NULL);
    edge_ws_hub_destroy(hub);
    edge_state_destroy(st);
    printf("  PASS: scrape once → state + WS\n");
}

static void test_notify_put_delete(void)
{
    edge_state_store_t *st = edge_state_create();
    edge_ws_hub_t *hub = edge_ws_hub_create(4);
    char buf[256];
    size_t n = 0;
    char msg[EDGE_WS_MSG_MAX];
    size_t mlen = 0;
    const char *put =
        "{\"ns\":\"map.dynamic\",\"key\":\"feature/fiber/1\",\"op\":\"put\","
        "\"value\":{\"id\":\"1\",\"status\":\"down\"},\"request_id\":\"n1\"}";
    const char *del =
        "{\"ns\":\"map.dynamic\",\"key\":\"feature/fiber/1\",\"op\":\"delete\","
        "\"request_id\":\"n2\"}";

    assert(st && hub);
    assert(edge_ws_hub_subscribe(hub, 0) == 0);
    assert(edge_notify_apply(st, hub, put, strlen(put)) == EDGE_NOTIFY_OK);
    assert(edge_state_get(st, "map.dynamic", "feature/fiber/1", buf, sizeof(buf),
                          &n) == EDGE_STATE_OK);
    assert(strstr(buf, "down") != NULL);
    assert(edge_ws_hub_take_pending(hub, 0, msg, sizeof(msg), &mlen) == 1);
    assert(strstr(msg, "\"op\":\"put\"") != NULL);

    assert(edge_notify_apply(st, hub, del, strlen(del)) == EDGE_NOTIFY_OK);
    assert(edge_state_get(st, "map.dynamic", "feature/fiber/1", buf, sizeof(buf),
                          &n) == EDGE_STATE_NOT_FOUND);
    assert(edge_ws_hub_take_pending(hub, 0, msg, sizeof(msg), &mlen) == 1);
    assert(strstr(msg, "\"op\":\"delete\"") != NULL);

    assert(edge_notify_apply(st, hub, "{\"op\":\"put\"}", 12) ==
           EDGE_NOTIFY_BAD_SCHEMA);
    assert(edge_notify_apply(st, hub, "!!!", 3) == EDGE_NOTIFY_BAD_JSON);

    edge_ws_hub_destroy(hub);
    edge_state_destroy(st);
    printf("  PASS: NOTIFY put/delete + schema\n");
}

int main(void)
{
    printf("edgehost_sidecar_notify_test:\n");
    test_parse_metrics();
    test_scrape_once();
    test_notify_put_delete();
    printf("All sidecar/notify tests PASSED\n");
    return 0;
}
