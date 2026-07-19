/**
 * P1.7c–d: RBAC, lab session, proxy X-User HMAC, HTTP gates.
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

    snprintf(cfg.auth_mode, sizeof(cfg.auth_mode), "proxy_headers");
    unsetenv("EDGEHOST_PROXY_HMAC");
    assert(edge_auth_ctx_from_config(&cfg, &auth, err, sizeof(err)) != 0);
    setenv("EDGEHOST_PROXY_HMAC", "proxy-secret-key", 1);
    assert(edge_auth_ctx_from_config(&cfg, &auth, err, sizeof(err)) == 0);
    assert(auth.mode == EDGE_AUTH_MODE_PROXY_HEADERS);
    assert(auth.proxy_hmac_key_len == strlen("proxy-secret-key"));
    unsetenv("EDGEHOST_PROXY_HMAC");
    printf("  PASS: auth_host env resolve\n");
}

static void test_proxy_sign_verify(void)
{
    edge_auth_ctx_t ctx;
    edge_principal_t p;
    char sig[EDGE_AUTH_PROXY_SIG_MAX];
    char bad[EDGE_AUTH_PROXY_SIG_MAX];

    edge_auth_ctx_init(&ctx);
    ctx.mode = EDGE_AUTH_MODE_PROXY_HEADERS;
    ctx.now_sec_override = 1700000000;
    ctx.proxy_max_skew_s = 60;
    ctx.proxy_hmac_key_len = 11;
    memcpy(ctx.proxy_hmac_key, "proxy-secret", 11);

    assert(edge_auth_proxy_sign(&ctx, "alice", "employee,employee_admin",
                                1700000000, sig, sizeof(sig)) == 0);
    assert(edge_auth_proxy_verify(&ctx, "alice", "employee,employee_admin",
                                  "1700000000", sig, &p) == 0);
    assert(p.authenticated);
    assert(strcmp(p.sub, "alice") == 0);
    assert(edge_auth_role_has(p.roles, EDGE_ROLE_EMPLOYEE));
    assert(edge_auth_role_has(p.roles, EDGE_ROLE_EMPLOYEE_ADMIN));

    /* default roles when header omitted */
    assert(edge_auth_proxy_sign(&ctx, "bob", NULL, 1700000000, sig,
                                sizeof(sig)) == 0);
    assert(edge_auth_proxy_verify(&ctx, "bob", NULL, "1700000000", sig, &p) ==
           0);
    assert(edge_auth_role_has(p.roles, EDGE_ROLE_EMPLOYEE));

    /* bad sig */
    snprintf(bad, sizeof(bad), "%s", sig);
    bad[0] = (char)(bad[0] ^ 1);
    assert(edge_auth_proxy_verify(&ctx, "bob", NULL, "1700000000", bad, &p) !=
           0);

    /* skew */
    ctx.now_sec_override = 1700000000 + 120;
    assert(edge_auth_proxy_verify(&ctx, "bob", NULL, "1700000000", sig, &p) !=
           0);

    assert(edge_auth_roles_from_csv("ingest") == EDGE_ROLE_INGEST);
    assert(edge_auth_mode_enforced(EDGE_AUTH_MODE_PROXY_HEADERS));
    printf("  PASS: proxy sign/verify/skew\n");
}

static void test_http_proxy_headers(void)
{
    edge_auth_ctx_t auth;
    edge_state_store_t *st = edge_state_create();
    edge_http1_serve_t *s = edge_http1_serve_create();
    edge_metrics_t m;
    char resp[2048];
    char sig[EDGE_AUTH_PROXY_SIG_MAX];
    char req[1024];
    size_t rlen = 0;
    int rc;
    int64_t ts = 1700000500;

    edge_metrics_init(&m);
    edge_auth_ctx_init(&auth);
    auth.mode = EDGE_AUTH_MODE_PROXY_HEADERS;
    auth.now_sec_override = ts;
    auth.proxy_max_skew_s = 300;
    auth.proxy_hmac_key_len = 10;
    memcpy(auth.proxy_hmac_key, "proxy-hmac", 10);

    assert(st && s);
    edge_http1_serve_set_state(s, st);
    edge_http1_serve_set_auth(s, &auth);

    /* no headers → 401 */
    rc = feed(s,
              "GET /api/v1/state/net.core/k HTTP/1.1\r\nHost: t\r\n\r\n", resp,
              sizeof(resp), &rlen, &m);
    assert(rc == 1);
    assert(strstr(resp, "401") != NULL);

    assert(edge_auth_proxy_sign(&auth, "carol", "employee", ts, sig,
                                sizeof(sig)) == 0);

    /* signed headers → PUT ok */
    edge_http1_serve_reset(s);
    edge_http1_serve_set_state(s, st);
    edge_http1_serve_set_auth(s, &auth);
    {
        int n = snprintf(
            req, sizeof(req),
            "PUT /api/v1/state/net.core/router/r2 HTTP/1.1\r\n"
            "Host: t\r\n"
            "X-User: carol\r\n"
            "X-Roles: employee\r\n"
            "X-Auth-Timestamp: %lld\r\n"
            "X-Auth-Signature: %s\r\n"
            "Content-Length: 25\r\n"
            "\r\n"
            "{\"id\":\"r2\",\"status\":\"ok\"}",
            (long long)ts, sig);
        assert(n > 0 && (size_t)n < sizeof(req));
        rlen = 0;
        rc = feed(s, req, resp, sizeof(resp), &rlen, &m);
        assert(rc == 1);
        assert(strstr(resp, "204") != NULL);
    }

    /* GET with default roles (no X-Roles header) */
    edge_http1_serve_reset(s);
    edge_http1_serve_set_state(s, st);
    edge_http1_serve_set_auth(s, &auth);
    {
        int n;
        assert(edge_auth_proxy_sign(&auth, "carol", NULL, ts, sig,
                                    sizeof(sig)) == 0);
        n = snprintf(req, sizeof(req),
                     "GET /api/v1/state/net.core/router/r2 HTTP/1.1\r\n"
                     "Host: t\r\n"
                     "X-User: carol\r\n"
                     "X-Auth-Timestamp: %lld\r\n"
                     "X-Auth-Signature: %s\r\n"
                     "\r\n",
                     (long long)ts, sig);
        assert(n > 0);
        rlen = 0;
        rc = feed(s, req, resp, sizeof(resp), &rlen, &m);
        assert(rc == 1);
        assert(strstr(resp, "200") != NULL);
        assert(strstr(resp, "r2") != NULL);
    }

    /* forged user with reused sig → 401 */
    edge_http1_serve_reset(s);
    edge_http1_serve_set_state(s, st);
    edge_http1_serve_set_auth(s, &auth);
    {
        int n = snprintf(req, sizeof(req),
                         "GET /api/v1/state/net.core/router/r2 HTTP/1.1\r\n"
                         "Host: t\r\n"
                         "X-User: evil\r\n"
                         "X-Auth-Timestamp: %lld\r\n"
                         "X-Auth-Signature: %s\r\n"
                         "\r\n",
                         (long long)ts, sig);
        assert(n > 0);
        rlen = 0;
        rc = feed(s, req, resp, sizeof(resp), &rlen, &m);
        assert(rc == 1);
        assert(strstr(resp, "401") != NULL);
    }

    edge_http1_serve_destroy(s);
    edge_state_destroy(st);
    printf("  PASS: HTTP proxy headers\n");
}

static void test_e7_classify_and_rbac(void)
{
    edge_principal_t emp, admin;

    assert(edge_auth_classify("GET", "/api/v1/e7/status", 0) == EDGE_RES_E7_GET);
    assert(edge_auth_classify("GET", "/api/v1/e7/shelves", 0) ==
           EDGE_RES_E7_GET);
    assert(edge_auth_classify("GET", "/api/v1/e7/shelves/00:02:5d:d9:21:47",
                              0) == EDGE_RES_E7_GET);
    assert(edge_auth_classify("GET",
                              "/api/v1/e7/shelves/00-02-5d-d9-21-47/commands/c1",
                              0) == EDGE_RES_E7_GET);
    assert(edge_auth_classify("GET", "/api/v1/e7/shelves/aa:bb:cc:dd:ee:ff/onts",
                              0) == EDGE_RES_E7_GET);
    assert(edge_auth_classify("PUT", "/api/v1/e7/shelves/00:02:5d:d9:21:47",
                              0) == EDGE_RES_E7_ADMIN);
    assert(edge_auth_classify("DELETE", "/api/v1/e7/shelves/00:02:5d:d9:21:47",
                              0) == EDGE_RES_E7_ADMIN);
    assert(edge_auth_classify("POST",
                              "/api/v1/e7/shelves/00:02:5d:d9:21:47/disconnect",
                              0) == EDGE_RES_E7_ADMIN);
    assert(edge_auth_classify("POST",
                              "/api/v1/e7/shelves/00:02:5d:d9:21:47/commands",
                              0) == EDGE_RES_E7_ADMIN);
    assert(edge_auth_classify("PATCH", "/api/v1/e7/status", 0) == EDGE_RES_NONE);

    edge_principal_clear(&emp);
    emp.authenticated = 1;
    emp.roles = EDGE_ROLE_EMPLOYEE;
    assert(edge_auth_rbac_check(&emp, EDGE_RES_E7_GET, NULL, NULL) ==
           EDGE_AUTH_ALLOW);
    assert(edge_auth_rbac_check(&emp, EDGE_RES_E7_ADMIN, NULL, NULL) ==
           EDGE_AUTH_DENY);

    edge_principal_clear(&admin);
    admin.authenticated = 1;
    admin.roles = EDGE_ROLE_EMPLOYEE_ADMIN;
    assert(edge_auth_rbac_check(&admin, EDGE_RES_E7_GET, NULL, NULL) ==
           EDGE_AUTH_ALLOW);
    assert(edge_auth_rbac_check(&admin, EDGE_RES_E7_ADMIN, NULL, NULL) ==
           EDGE_AUTH_ALLOW);

    printf("  PASS: e7 classify + rbac employee vs admin\n");
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
    test_proxy_sign_verify();
    test_http_proxy_headers();
    test_e7_classify_and_rbac();
    printf("All auth tests PASSED\n");
    return 0;
}
