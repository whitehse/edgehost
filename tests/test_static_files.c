/**
 * P1.6: static resolve, SPA + packages HTTP serve.
 */
#include "edge_config.h"
#include "edge_http1_serve.h"
#include "edge_metrics.h"
#include "edge_static.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_resolve_safe(void)
{
    char out[512];

    assert(edge_static_resolve("./spa", "/index.html", out, sizeof(out)) == 0);
    assert(strstr(out, "index.html") != NULL);

    assert(edge_static_resolve("./spa", "/", out, sizeof(out)) == 0);
    assert(strstr(out, "index.html") != NULL);

    assert(edge_static_resolve("./spa", "/../etc/passwd", out, sizeof(out)) ==
           -1);
    assert(edge_static_resolve("./spa", "/foo/../../etc", out, sizeof(out)) ==
           -1);
    assert(edge_static_resolve("./spa", "/a/./b", out, sizeof(out)) == -1);

    assert(edge_static_resolve("./packages", "/demo.wmap", out, sizeof(out)) ==
           0);
    assert(strstr(out, "demo.wmap") != NULL);
    printf("  PASS: resolve safe\n");
}

static void test_load_spa_fixture(void)
{
    char buf[8192];
    size_t n = 0;
    char ctype[64];

    assert(edge_static_load("spa", "/index.html", buf, sizeof(buf), &n, ctype,
                            sizeof(ctype)) == 0);
    assert(n > 0);
    assert(strstr(buf, "edgehost") != NULL);
    assert(strstr(ctype, "text/html") != NULL);

    assert(edge_static_load("spa", "/app.js", buf, sizeof(buf), &n, ctype,
                            sizeof(ctype)) == 0);
    assert(strstr(ctype, "javascript") != NULL);

    /* Directory URL → index.html (status map shell) */
    assert(edge_static_load("spa", "/map/", buf, sizeof(buf), &n, ctype,
                            sizeof(ctype)) == 0);
    assert(n > 0);
    assert(strstr(buf, "status map") != NULL || strstr(buf, "map_boot") != NULL);
    assert(strstr(ctype, "text/html") != NULL);

    printf("  PASS: load spa fixtures\n");
}

static void test_load_package(void)
{
    char buf[256];
    size_t n = 0;
    char ctype[64];

    assert(edge_static_load("packages", "/demo.wmap", buf, sizeof(buf), &n,
                            ctype, sizeof(ctype)) == 0);
    assert(n > 0);
    assert(strstr(buf, "WMAP-DEMO") != NULL);
    printf("  PASS: load package fixture\n");
}

static int feed_get(edge_http1_serve_t *s, const char *path, char *resp,
                    size_t resp_cap, size_t *rlen, edge_metrics_t *m)
{
    char req[256];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: t\r\n\r\n", path);
    assert(n > 0);
    return edge_http1_serve_feed(s, (const uint8_t *)req, (size_t)n, m, resp,
                                 resp_cap, rlen);
}

static void test_http_spa_and_packages(void)
{
    edge_http1_serve_t *s = edge_http1_serve_create();
    edge_http1_docroot_t roots;
    edge_metrics_t m;
    char resp[8192];
    size_t rlen = 0;
    int rc;

    edge_metrics_init(&m);
    memset(&roots, 0, sizeof(roots));
    roots.spa_root = "spa";
    roots.packages_root = "packages";
    roots.max_file_bytes = 64 * 1024;
    edge_http1_serve_set_docroots(s, &roots);

    rc = feed_get(s, "/", resp, sizeof(resp), &rlen, &m);
    assert(rc == 1);
    assert(strstr(resp, "HTTP/1.1 200") != NULL);
    assert(strstr(resp, "text/html") != NULL);
    assert(strstr(resp, "edgehost") != NULL);

    edge_http1_serve_reset(s);
    edge_http1_serve_set_docroots(s, &roots);
    rc = feed_get(s, "/app.js", resp, sizeof(resp), &rlen, &m);
    assert(rc == 1);
    assert(strstr(resp, "200") != NULL);
    assert(strstr(resp, "javascript") != NULL);

    edge_http1_serve_reset(s);
    edge_http1_serve_set_docroots(s, &roots);
    rc = feed_get(s, "/packages/demo.wmap", resp, sizeof(resp), &rlen, &m);
    assert(rc == 1);
    assert(strstr(resp, "200") != NULL);
    assert(strstr(resp, "WMAP-DEMO") != NULL);

    edge_http1_serve_reset(s);
    edge_http1_serve_set_docroots(s, &roots);
    rc = feed_get(s, "/no-such-file.xyz", resp, sizeof(resp), &rlen, &m);
    assert(rc == 1);
    /* SPA fallback to index.html for client routers */
    assert(strstr(resp, "200") != NULL || strstr(resp, "404") != NULL);

    edge_http1_serve_destroy(s);
    printf("  PASS: HTTP SPA + packages\n");
}

static void test_yaml_packages_key(void)
{
    edge_config_t cfg;
    char err[64];
    const char *yaml =
        "packages:\n"
        "  root: /var/edge/packages\n"
        "spa:\n"
        "  root: /var/edge/spa\n"
        "  max_file_bytes: 8192\n";

    assert(edge_config_load_yaml_buf(yaml, strlen(yaml), &cfg, err,
                                     sizeof(err)) == 0);
    assert(strcmp(cfg.packages_root, "/var/edge/packages") == 0);
    assert(strcmp(cfg.spa_root, "/var/edge/spa") == 0);
    assert(cfg.static_max_file_bytes == 8192);
    printf("  PASS: YAML packages + spa max_file\n");
}

int main(void)
{
    printf("static_files:\n");
    test_resolve_safe();
    test_load_spa_fixture();
    test_load_package();
    test_http_spa_and_packages();
    test_yaml_packages_key();
    printf("all passed\n");
    return 0;
}
