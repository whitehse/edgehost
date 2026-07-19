/**
 * P1.5: class-A sim_main drive smoke (no libFuzzer required).
 */
#include "edge_http1_serve.h"
#include "edge_metrics.h"
#include "edge_sim_main.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_http1_serve_health(void)
{
    edge_http1_serve_t *s = edge_http1_serve_create();
    edge_metrics_t m;
    char resp[1024];
    size_t rlen = 0;
    const char *req = "GET /health HTTP/1.1\r\nHost: t\r\n\r\n";
    int rc;

    edge_metrics_init(&m);
    assert(s);
    rc = edge_http1_serve_feed(s, (const uint8_t *)req, strlen(req), &m, resp,
                               sizeof(resp), &rlen);
    assert(rc == 1);
    assert(rlen > 0);
    assert(strstr(resp, "HTTP/1.1 200") != NULL);
    assert(strstr(resp, "application/json") != NULL);
    assert(strstr(resp, "\"status\":\"ok\"") != NULL);
    assert(m.requests == 1);
    assert(m.responses_2xx == 1);
    edge_http1_serve_destroy(s);
    printf("  PASS: http1_serve health\n");
}

static void test_http1_serve_partial(void)
{
    edge_http1_serve_t *s = edge_http1_serve_create();
    edge_metrics_t m;
    char resp[1024];
    size_t rlen = 0;
    int rc;

    edge_metrics_init(&m);
    assert(s);
    rc = edge_http1_serve_feed(s, (const uint8_t *)"GET / HTTP/1.1\r\n", 16, &m,
                               resp, sizeof(resp), &rlen);
    assert(rc == 0);
    rc = edge_http1_serve_feed(s, (const uint8_t *)"Host: x\r\n\r\n", 11, &m,
                               resp, sizeof(resp), &rlen);
    assert(rc == 1);
    assert(strstr(resp, "200") != NULL);
    edge_http1_serve_destroy(s);
    printf("  PASS: http1_serve partial\n");
}

static void test_sim_drive_empty(void)
{
    assert(edge_sim_drive(NULL, 0) == 0);
    assert(edge_sim_drive((const uint8_t *)"", 0) == 0);
    printf("  PASS: sim_drive empty\n");
}

static void test_sim_drive_health_bytes(void)
{
    static const uint8_t junk[] = {
        0x01, 0x02, 0x80, 0x01, /* mixed sim opcodes */
        'G',  'E',  'T',  ' ',  '/', 'h', 'e', 'a', 'l', 't', 'h', ' ',
        'H',  'T',  'T',  'P',  '/', '1', '.', '1', '\r', '\n',
        'H',  'o',  's',  't',  ':', ' ', 't', '\r', '\n', '\r', '\n'
    };
    assert(edge_sim_drive(junk, sizeof(junk)) == 0);
    printf("  PASS: sim_drive adversarial + health-like\n");
}

static void test_sim_drive_many(void)
{
    uint8_t buf[256];
    size_t i;
    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(i * 17u + 3u);
    }
    assert(edge_sim_drive(buf, sizeof(buf)) == 0);
    printf("  PASS: sim_drive patterned 256 bytes\n");
}

int main(void)
{
    printf("sim_main:\n");
    test_http1_serve_health();
    test_http1_serve_partial();
    test_sim_drive_empty();
    test_sim_drive_health_bytes();
    test_sim_drive_many();
    printf("all passed\n");
    return 0;
}
