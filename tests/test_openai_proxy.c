/**
 * P1.8b: openai_proxy PENDING + outbound HTTP against local mock upstream.
 */
#include "edge_http1_serve.h"
#include "edge_metrics.h"
#include "edge_openai_proxy.h"
#include "edge_outbound.h"
#include "edge_plugin_host.h"
#include "edge_state.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int port;
    int ready;
    int stop;
} mock_srv_t;

static void *mock_upstream_thread(void *arg)
{
    mock_srv_t *ms = (mock_srv_t *)arg;
    int lfd;
    int on = 1;
    struct sockaddr_in addr;

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(lfd >= 0);
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    {
        socklen_t alen = sizeof(addr);
        assert(getsockname(lfd, (struct sockaddr *)&addr, &alen) == 0);
        ms->port = ntohs(addr.sin_port);
    }
    assert(listen(lfd, 8) == 0);
    ms->ready = 1;

    while (!ms->stop) {
        struct timeval tv;
        fd_set rfds;
        int cfd;
        char req[8192];
        ssize_t n;
        const char *body =
            "{\"id\":\"chatcmpl-test\",\"object\":\"chat.completion\","
            "\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hi\"}}]}";
        char resp[1024];
        int rn;

        FD_ZERO(&rfds);
        FD_SET(lfd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        if (select(lfd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            continue;
        }
        cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            continue;
        }
        n = read(cfd, req, sizeof(req) - 1);
        if (n < 0) {
            n = 0;
        }
        req[n] = '\0';
        /* require Authorization and path */
        assert(strstr(req, "Authorization: Bearer") != NULL);
        assert(strstr(req, "/v1/chat/completions") != NULL);
        rn = snprintf(resp, sizeof(resp),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "%s",
                      strlen(body), body);
        (void)write(cfd, resp, (size_t)rn);
        close(cfd);
    }
    close(lfd);
    return NULL;
}

static void test_outbound_plain(void)
{
    mock_srv_t ms;
    pthread_t th;
    edge_http_client_req_t req;
    edge_http_client_result_t res;
    edge_outbound_opts_t opts;
    uint8_t body[4096];
    size_t blen = 0;
    char url[128];
    char addr[32];

    memset(&ms, 0, sizeof(ms));
    assert(pthread_create(&th, NULL, mock_upstream_thread, &ms) == 0);
    while (!ms.ready) {
        usleep(1000);
    }
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/chat/completions",
             ms.port);
    snprintf(addr, sizeof(addr), "127.0.0.1");

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.url = url;
    req.addr_override = addr;
    {
        static const char *hn[] = {"Authorization", "Content-Type"};
        static const char *hv[] = {"Bearer sk-test", "application/json"};
        static const char body_in[] = "{\"model\":\"gpt-test\",\"messages\":[]}";
        req.hdr_names = hn;
        req.hdr_values = hv;
        req.n_headers = 2;
        req.body = (const uint8_t *)body_in;
        req.body_len = sizeof(body_in) - 1;
    }
    edge_outbound_opts_defaults(&opts);
    opts.allow_blocking_dns = 0;
    assert(edge_outbound_http_execute(&req, &opts, &res, body, sizeof(body),
                                      &blen) == 0);
    assert(res.transport_err == 0);
    assert(res.status == 200);
    assert(strstr((char *)body, "chat.completion") != NULL);

    ms.stop = 1;
    pthread_join(th, NULL);
    printf("  PASS: outbound plain HTTP\n");
}

static void test_plugin_e2e_via_serve(void)
{
    mock_srv_t ms;
    pthread_t th;
    edge_state_store_t *st;
    edge_plugin_host_t *ph;
    edge_plugin_host_config_t phc;
    edge_plugin_t plugin;
    edge_openai_proxy_config_t ocfg, ostore;
    edge_http1_serve_t *srv;
    edge_metrics_t m;
    char resp[8192];
    size_t rlen = 0;
    char req[2048];
    int rc;
    const char *body =
        "{\"model\":\"gpt-test\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";

    memset(&ms, 0, sizeof(ms));
    assert(pthread_create(&th, NULL, mock_upstream_thread, &ms) == 0);
    while (!ms.ready) {
        usleep(1000);
    }

    setenv("OPENAI_API_KEY", "sk-test-key", 1);
    edge_openai_proxy_config_defaults(&ocfg);
    ocfg.enabled = 1;
    snprintf(ocfg.upstream, sizeof(ocfg.upstream), "http://127.0.0.1:%d",
             ms.port);
    snprintf(ocfg.upstream_addr, sizeof(ocfg.upstream_addr), "127.0.0.1");
    ocfg.timeout_ms = 5000;
    ocfg.rate_limit_rpm = 0;

    st = edge_state_create();
    memset(&phc, 0, sizeof(phc));
    phc.state = st;
    phc.max_pending = 8;
    ph = edge_plugin_host_create(&phc);
    assert(ph);
    assert(edge_openai_proxy_init_plugin(&plugin, &ostore, &ocfg) == 0);
    assert(edge_plugin_host_register(ph, &plugin, NULL) == 0);
    assert(edge_plugin_host_add_route(ph, "/v1", &plugin) == 0);

    srv = edge_http1_serve_create();
    edge_metrics_init(&m);
    edge_http1_serve_set_plugin_host(srv, ph);
    edge_http1_serve_set_outbound_policy(srv, 0, 256 * 1024);

    {
        int n = snprintf(req, sizeof(req),
                         "POST /v1/chat/completions HTTP/1.1\r\n"
                         "Host: t\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: %zu\r\n"
                         "\r\n"
                         "%s",
                         strlen(body), body);
        assert(n > 0);
        rc = edge_http1_serve_feed(srv, (const uint8_t *)req, (size_t)n, &m,
                                   resp, sizeof(resp), &rlen);
        assert(rc == 1);
        assert(strstr(resp, "200") != NULL);
        assert(strstr(resp, "chat.completion") != NULL);
        assert(strstr(resp, "\"content\":\"hi\"") != NULL);
    }

    /* unknown /v1 route → 404 from plugin */
    edge_http1_serve_reset(srv);
    edge_http1_serve_set_plugin_host(srv, ph);
    edge_http1_serve_set_outbound_policy(srv, 0, 256 * 1024);
    {
        const char *bad =
            "POST /v1/unknown HTTP/1.1\r\nHost: t\r\n"
            "Content-Length: 2\r\n\r\n{}";
        rlen = 0;
        rc = edge_http1_serve_feed(srv, (const uint8_t *)bad, strlen(bad), &m,
                                   resp, sizeof(resp), &rlen);
        assert(rc == 1);
        assert(strstr(resp, "404") != NULL);
    }

    edge_http1_serve_destroy(srv);
    edge_plugin_host_destroy(ph);
    edge_state_destroy(st);
    ms.stop = 1;
    pthread_join(th, NULL);
    unsetenv("OPENAI_API_KEY");
    printf("  PASS: openai_proxy E2E via serve (mock upstream)\n");
}

static void test_service_bearer(void)
{
    mock_srv_t ms;
    pthread_t th;
    edge_state_store_t *st;
    edge_plugin_host_t *ph;
    edge_plugin_host_config_t phc;
    edge_plugin_t plugin;
    edge_openai_proxy_config_t ocfg, ostore;
    edge_http1_serve_t *srv;
    edge_auth_ctx_t auth;
    edge_metrics_t m;
    char resp[4096];
    size_t rlen = 0;
    char req[1024];
    const char *body = "{\"model\":\"m\",\"messages\":[]}";

    memset(&ms, 0, sizeof(ms));
    assert(pthread_create(&th, NULL, mock_upstream_thread, &ms) == 0);
    while (!ms.ready) {
        usleep(1000);
    }
    setenv("OPENAI_API_KEY", "sk-up", 1);
    edge_openai_proxy_config_defaults(&ocfg);
    ocfg.enabled = 1;
    snprintf(ocfg.upstream, sizeof(ocfg.upstream), "http://127.0.0.1:%d",
             ms.port);
    snprintf(ocfg.upstream_addr, sizeof(ocfg.upstream_addr), "127.0.0.1");
    ocfg.rate_limit_rpm = 0;

    st = edge_state_create();
    memset(&phc, 0, sizeof(phc));
    phc.state = st;
    ph = edge_plugin_host_create(&phc);
    assert(edge_openai_proxy_init_plugin(&plugin, &ostore, &ocfg) == 0);
    assert(edge_plugin_host_register(ph, &plugin, NULL) == 0);
    assert(edge_plugin_host_add_route(ph, "/v1", &plugin) == 0);

    edge_auth_ctx_init(&auth);
    auth.mode = EDGE_AUTH_MODE_LAB_PASSWORD;
    snprintf(auth.lab_password, sizeof(auth.lab_password), "x");
    auth.hmac_key_len = 4;
    memcpy(auth.hmac_key, "key!", 4);

    srv = edge_http1_serve_create();
    edge_metrics_init(&m);
    edge_http1_serve_set_auth(srv, &auth);
    edge_http1_serve_set_plugin_host(srv, ph);
    edge_http1_serve_set_outbound_policy(srv, 0, 65536);
    edge_http1_serve_set_service_api_key(srv, "svc-secret");

    /* no auth → 401 */
    {
        int n = snprintf(req, sizeof(req),
                         "POST /v1/chat/completions HTTP/1.1\r\n"
                         "Host: t\r\nContent-Length: %zu\r\n\r\n%s",
                         strlen(body), body);
        assert(edge_http1_serve_feed(srv, (const uint8_t *)req, (size_t)n, &m,
                                     resp, sizeof(resp), &rlen) == 1);
        assert(strstr(resp, "401") != NULL);
    }

    edge_http1_serve_reset(srv);
    edge_http1_serve_set_auth(srv, &auth);
    edge_http1_serve_set_plugin_host(srv, ph);
    edge_http1_serve_set_outbound_policy(srv, 0, 65536);
    edge_http1_serve_set_service_api_key(srv, "svc-secret");
    {
        int n = snprintf(req, sizeof(req),
                         "POST /v1/chat/completions HTTP/1.1\r\n"
                         "Host: t\r\n"
                         "Authorization: Bearer svc-secret\r\n"
                         "Content-Length: %zu\r\n\r\n%s",
                         strlen(body), body);
        rlen = 0;
        assert(edge_http1_serve_feed(srv, (const uint8_t *)req, (size_t)n, &m,
                                     resp, sizeof(resp), &rlen) == 1);
        assert(strstr(resp, "200") != NULL);
    }

    edge_http1_serve_destroy(srv);
    edge_plugin_host_destroy(ph);
    edge_state_destroy(st);
    ms.stop = 1;
    pthread_join(th, NULL);
    unsetenv("OPENAI_API_KEY");
    printf("  PASS: service bearer → service_openai\n");
}

int main(void)
{
    printf("edgehost_openai_test:\n");
    test_outbound_plain();
    test_plugin_e2e_via_serve();
    test_service_bearer();
    printf("All openai_proxy tests PASSED\n");
    return 0;
}
