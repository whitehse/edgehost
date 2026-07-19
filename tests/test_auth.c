/**
 * P1.7c: RBAC, lab session HMAC, POST /auth/lab-login, state gate.
 */
#include "edge_auth.h"
#include "edge_auth_host.h"
#include "edge_config.h"
#include "edge_http1_serve.h"
#include "edge_metrics.h"
#include "edge_state.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_rbac_employee(void)
{
    edge_principal_t p;
    edge_principal_clear(&p);
    assert(edge_auth_rbac_check(&p, EDGE_RES_STATE_GET, "net.core", "k") ==
           EDGE_AUTH_DENY);

    p.authenticated = 1;
    p.roles = EDGE_ROLE_EMPLOYEE;
    assert(edge_auth_rbac_check(&p, EDGE_RES_STATE_GET, "net.core", "k") ==
           EDGE_AUTH_ALLOW);
    assert(edge_auth_rbac_check(&p, EDGE_RES_STATE_PUT, "net.core", "k") ==
           EDGE_AUTH_ALLOW);
    assert(edge_auth_rbac_check(&p, EDGE_RES_WS_STREAM, NULL, NULL) ==
           EDGE_AUTH_ALLOW);

    p.roles = EDGE_ROLE_INGEST;
    assert(edge_auth_rbac_check(&p, EDGE_RES_STATE_PUT, "net.core", "k") ==
           EDGE_AUTH_ALLOW);
    assert(edge_auth_rbac_check(&p, EDGE_RES_STATE_GET, "net.core", "k") ==
           EDGE_AUTH_DENY);
    printf("  PASS: rbac employee/ingest\n");
}

static void test_session_roundtrip(void)
{
    edge_auth_ctx_t ctx;
    edge_principal_t a, b;
    char cookie[EDGE_AUTH_COOKIE_MAX];

    edge_auth_ctx_init(&ctx);
    ctx.mode = EDGE_AUTH_MODE_LAB_PASSWORD;
    ctx.now_sec_override = 1700000000;
    ctx.session_ttl_s = 3600;
    snprintf(ctx.lab_password, sizeof(ctx.lab_password), "s3cret");
    ctx.hmac_key_len = 12;
    memcpy(ctx.hmac_key, "hmac-key-lab", 12);

    assert(edge_auth_session_issue(&ctx, "lab", EDGE_ROLE_EMPLOYEE, cookie,
                                   sizeof(cookie), &a) == 0);
    assert(a.authenticated);
    assert(a.exp == 1700000000 + 3600);
    assert(strchr(cookie, '.') != NULL);

    assert(edge_auth_session_verify(&ctx, cookie, &b) == 0);
    assert(b.authenticated);
    assert(strcmp(b.sub, "lab") == 0);
    assert(edge_auth_role_has(b.roles, EDGE_ROLE_EMPLOYEE));
    assert(b.exp == a.exp);

    /* tamper */
    cookie[0] = (char)(cookie[0] ^ 1);
    assert(edge_auth_session_verify(&ctx, cookie, &b) != 0);

    /* restore and expire */
    cookie[0] = (char)(cookie[0] ^ 1);
    ctx.now_sec_override = a.exp + 1;
    assert(edge_auth_session_verify(&ctx, cookie, &b) != 0);
    printf("  PASS: session issue/verify/exp\n");
}

static void test_password_and_cookie_extract(void)
{
    edge_auth_ctx_t ctx;
    char out[64];
    edge_auth_ctx_init(&ctx);
    snprintf(ctx.lab_password, sizeof(ctx.lab_password), "pw");
    assert(edge_auth_password_ok(&ctx, "pw", 2));
    assert(!edge_auth_password_ok(&ctx, "px", 2));
    assert(!edge_auth_password_ok(&ctx, "p", 1));

    assert(edge_auth_cookie_extract("a=1; edge_session=abc.def; b=2", out,
                                    sizeof(out)) == 0);
    assert(strcmp(out, "abc.def") == 0);
    assert(edge_auth_cookie_extract("session=nope", out, sizeof(out)) != 0);

    assert(edge_auth_parse_login_password("{\"password\":\"hello\"}", 20, out,
                                          sizeof(out)) == 0);
    assert(strcmp(out, "hello") == 0);
    printf("  PASS: password + cookie extract\n");
}

static void test_config_auth_yaml(void)
{
    const char *yaml =
        "auth:\n"
        "  mode: lab_password\n"
        "  lab_password_env: MY_PW\n"
        "  session_hmac_key_env: MY_HMAC\n"
        "  session_ttl_s: 60\n";
    edge_config_t cfg;
    char err[128];
    assert(edge_config_load_yaml_buf(yaml, strlen(yaml), &cfg, err,
                                     sizeof(err)) == 0);
    assert(strcmp(cfg.auth_mode, "lab_password") == 0);
    assert(strcmp(cfg.auth_lab_password_env, "MY_PW") == 0);
    assert(strcmp(cfg.auth_session_hmac_key_env, "MY_HMAC") == 0);
    assert(cfg.auth_session_ttl_s == 60);
    printf("  PASS: auth yaml fields\n");
}

static int feed(edge_http1_serve_t *s, const char *req, char *resp, size_t cap,
                size_t *rlen, edge_metrics_t *m)
{
    return edge_http1_serve_feed(s, (const uint8_t *)req, strlen(req), m, resp,
                                 cap, rlen);
}

static void test_http_login_and_state(void)
{
    edge_auth_ctx_t auth;
    edge_state_store_t *st = edge_state_create();
    edge_http1_serve_t *s = edge_http1_serve_create();
    edge_metrics_t m;
    char resp[2048];
    size_t rlen = 0;
    char cookie[EDGE_AUTH_COOKIE_MAX];
    const char *p;
    const char *end;
    char *setc;
    int rc;

    edge_metrics_init(&m);
    edge_auth_ctx_init(&auth);
    auth.mode = EDGE_AUTH_MODE_LAB_PASSWORD;
    auth.now_sec_override = 1700000000;
    auth.session_ttl_s = 7200;
    snprintf(auth.lab_password, sizeof(auth.lab_password), "lab-pass");
    auth.hmac_key_len = 8;
    memcpy(auth.hmac_key, "keykey12", 8);

    assert(st && s);
    edge_http1_serve_set_state(s, st);
    edge_http1_serve_set_auth(s, &auth);

    /* unauthenticated GET state → 401 */
    rc = feed(s,
              "GET /api/v1/state/net.core/r1 HTTP/1.1\r\nHost: t\r\n\r\n",
              resp, sizeof(resp), &rlen, &m);
    assert(rc == 1);
    assert(strstr(resp, "401") != NULL);
    assert(strstr(resp, "UNAUTHORIZED") != NULL);

    /* bad password */
    edge_http1_serve_reset(s);
    edge_http1_serve_set_state(s, st);
    edge_http1_serve_set_auth(s, &auth);
    {
        const char *bad =
            "POST /auth/lab-login HTTP/1.1\r\n"
            "Host: t\r\n"
            "Content-Length: 20\r\n"
            "\r\n"
            "{\"password\":\"wrong\"}";
        rlen = 0;
        rc = feed(s, bad, resp, sizeof(resp), &rlen, &m);
        assert(rc == 1);
        assert(strstr(resp, "401") != NULL);
        assert(strstr(resp, "BAD_PASSWORD") != NULL);
    }

    /* good login */
    edge_http1_serve_reset(s);
    edge_http1_serve_set_state(s, st);
    edge_http1_serve_set_auth(s, &auth);
    {
        const char *login =
            "POST /auth/lab-login HTTP/1.1\r\n"
            "Host: t\r\n"
            "Content-Length: 23\r\n"
            "\r\n"
            "{\"password\":\"lab-pass\"}";
        rlen = 0;
        rc = feed(s, login, resp, sizeof(resp), &rlen, &m);
        assert(rc == 1);
        assert(strstr(resp, "200") != NULL);
        assert(strstr(resp, "Set-Cookie: edge_session=") != NULL);
        assert(strstr(resp, "HttpOnly") != NULL);
        assert(strstr(resp, "\"ok\":true") != NULL);

        setc = strstr(resp, "Set-Cookie: edge_session=");
        assert(setc);
        p = setc + strlen("Set-Cookie: edge_session=");
        end = p;
        while (*end && *end != ';' && *end != '\r') {
            end++;
        }
        assert((size_t)(end - p) < sizeof(cookie));
        memcpy(cookie, p, (size_t)(end - p));
        cookie[end - p] = '\0';
    }

    /* PUT with cookie */
    edge_http1_serve_reset(s);
    edge_http1_serve_set_state(s, st);
    edge_http1_serve_set_auth(s, &auth);
    {
        char put[1024];
        int n = snprintf(put, sizeof(put),
                         "PUT /api/v1/state/net.core/router/r1 HTTP/1.1\r\n"
                         "Host: t\r\n"
                         "Cookie: edge_session=%s\r\n"
                         "Content-Length: 25\r\n"
                         "\r\n"
                         "{\"id\":\"r1\",\"status\":\"ok\"}",
                         cookie);
        assert(n > 0);
        rlen = 0;
        rc = feed(s, put, resp, sizeof(resp), &rlen, &m);
        assert(rc == 1);
        assert(strstr(resp, "204") != NULL);
    }

    /* GET with cookie */
    edge_http1_serve_reset(s);
    edge_http1_serve_set_state(s, st);
    edge_http1_serve_set_auth(s, &auth);
    {
        char get[1024];
        snprintf(get, sizeof(get),
                 "GET /api/v1/state/net.core/router/r1 HTTP/1.1\r\n"
                 "Host: t\r\n"
                 "Cookie: edge_session=%s\r\n"
                 "\r\n",
                 cookie);
        rlen = 0;
        rc = feed(s, get, resp, sizeof(resp), &rlen, &m);
        assert(rc == 1);
        assert(strstr(resp, "200") != NULL);
        assert(strstr(resp, "r1") != NULL);
    }

    /* /auth/me */
    edge_http1_serve_reset(s);
    edge_http1_serve_set_auth(s, &auth);
    {
        char me[1024];
        snprintf(me, sizeof(me),
                 "GET /auth/me HTTP/1.1\r\nHost: t\r\n"
                 "Cookie: edge_session=%s\r\n\r\n",
                 cookie);
        rlen = 0;
        rc = feed(s, me, resp, sizeof(resp), &rlen, &m);
        assert(rc == 1);
        assert(strstr(resp, "200") != NULL);
        assert(strstr(resp, "\"sub\":\"lab\"") != NULL);
    }

    /* WS without cookie → 401 */
    edge_http1_serve_reset(s);
    edge_http1_serve_set_auth(s, &auth);
    {
        const char *ws =
            "GET /api/v1/stream?topics=state HTTP/1.1\r\n"
            "Host: t\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";
        rlen = 0;
        rc = feed(s, ws, resp, sizeof(resp), &rlen, &m);
        assert(rc == 1);
        assert(strstr(resp, "401") != NULL);
    }

    /* open mode still allows state without cookie */
    edge_http1_serve_reset(s);
    edge_http1_serve_set_state(s, st);
    edge_http1_serve_set_auth(s, NULL);
    {
        rlen = 0;
        rc = feed(s,
                  "GET /api/v1/state/net.core/router/r1 HTTP/1.1\r\n"
                  "Host: t\r\n\r\n",
                  resp, sizeof(resp), &rlen, &m);
        assert(rc == 1);
        assert(strstr(resp, "200") != NULL);
    }

    edge_http1_serve_destroy(s);
    edge_state_destroy(st);
    printf("  PASS: HTTP login + state RBAC\n");
}

static void test_auth_host_from_env(void)
{
    edge_config_t cfg;
    edge_auth_ctx_t auth;
    char err[128];

    edge_config_defaults(&cfg);
    assert(edge_auth_ctx_from_config(&cfg, &auth, err, sizeof(err)) == 0);
    assert(auth.mode == EDGE_AUTH_MODE_OPEN);

    snprintf(cfg.auth_mode, sizeof(cfg.auth_mode), "lab_password");
    unsetenv("EDGEHOST_LAB_PASSWORD");
    unsetenv("EDGEHOST_SESSION_HMAC");
    assert(edge_auth_ctx_from_config(&cfg, &auth, err, sizeof(err)) != 0);

    setenv("EDGEHOST_LAB_PASSWORD", "p", 1);
    setenv("EDGEHOST_SESSION_HMAC", "secret-hmac-key", 1);
    assert(edge_auth_ctx_from_config(&cfg, &auth, err, sizeof(err)) == 0);
    assert(auth.mode == EDGE_AUTH_MODE_LAB_PASSWORD);
    assert(strcmp(auth.lab_password, "p") == 0);
    assert(auth.hmac_key_len == strlen("secret-hmac-key"));
    unsetenv("EDGEHOST_LAB_PASSWORD");
    unsetenv("EDGEHOST_SESSION_HMAC");
    printf("  PASS: auth_host env resolve\n");
}

int main(void)
{
    printf("edgehost_auth_test:\n");
    test_rbac_employee();
    test_session_roundtrip();
    test_password_and_cookie_extract();
    test_config_auth_yaml();
    test_http_login_and_state();
    test_auth_host_from_env();
    printf("All auth tests PASSED\n");
    return 0;
}
