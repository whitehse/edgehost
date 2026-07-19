/**
 * P1.13: OpenSSL non-blocking TLS server on io_uring (/health over HTTPS).
 */
#include "edge_config.h"
#include "edge_iouring.h"
#include "edge_metrics.h"
#include "edge_tls.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_self_signed(const char *cert, const char *key)
{
    char cmd[512];
    int rc;

    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 -keyout '%s' -out '%s' "
             "-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
             key, cert);
    rc = system(cmd);
    return rc == 0 ? 0 : -1;
}

static int https_get_health(uint16_t port, char *out, size_t out_sz)
{
    char cmd[256];
    FILE *fp;
    size_t n;

    snprintf(cmd, sizeof(cmd),
             "curl -sk --max-time 3 https://127.0.0.1:%u/health 2>/dev/null",
             (unsigned)port);
    fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }
    n = fread(out, 1, out_sz - 1, fp);
    out[n] = '\0';
    pclose(fp);
    return (int)n;
}

static void test_tls_ctx_helpers(void)
{
    char cert[] = "/tmp/edgehost_tls_test.crt";
    char key[] = "/tmp/edgehost_tls_test.key";
    edge_tls_config_t tc;
    SSL_CTX *ctx;

    assert(write_self_signed(cert, key) == 0);
    assert(edge_tls_config_enabled(cert, key));
    assert(!edge_tls_config_enabled("", key));
    memset(&tc, 0, sizeof(tc));
    tc.cert_file = cert;
    tc.key_file = key;
    ctx = edge_tls_ctx_create(&tc);
    assert(ctx);
    edge_tls_ctx_free(ctx);
    printf("  PASS: TLS ctx create\n");
}

static void test_tls_health_e2e(void)
{
    char cert[] = "/tmp/edgehost_tls_e2e.crt";
    char key[] = "/tmp/edgehost_tls_e2e.key";
    uint16_t port = (uint16_t)(21000 + (getpid() % 500));
    pid_t pid;
    char body[512];
    int n;
    int status;

    assert(write_self_signed(cert, key) == 0);

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        edge_config_t cfg;
        edge_iouring_opts_t opts;
        edge_config_defaults(&cfg);
        snprintf(cfg.listen_host, sizeof(cfg.listen_host), "127.0.0.1");
        cfg.listen_port = port;
        snprintf(cfg.tls_cert, sizeof(cfg.tls_cert), "%s", cert);
        snprintf(cfg.tls_key, sizeof(cfg.tls_key), "%s", key);
        /* clear spa so /health is hit without static roots issues */
        cfg.spa_root[0] = '\0';
        cfg.packages_root[0] = '\0';
        edge_iouring_opts_defaults(&opts);
        opts.max_accepts = 1;
        opts.max_conns = 8;
        _exit(edge_iouring_run(&cfg, &opts) == 0 ? 0 : 1);
    }

    /* wait for listen */
    usleep(300000);
    n = https_get_health(port, body, sizeof(body));
    if (n <= 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        fprintf(stderr, "TLS health fetch failed (n=%d)\n", n);
        assert(0);
    }
    assert(strstr(body, "{") != NULL);

    /* ensure child exits (one accept) */
    {
        int waited = 0;
        while (waitpid(pid, &status, WNOHANG) == 0 && waited < 50) {
            usleep(100000);
            waited++;
        }
        if (waitpid(pid, &status, WNOHANG) == 0) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            fprintf(stderr, "TLS server child did not exit\n");
            assert(0);
        }
    }
    assert(WIFEXITED(status));
    unlink(cert);
    unlink(key);
    printf("  PASS: HTTPS /health E2E\n");
}

int main(void)
{
    printf("edgehost_tls_test:\n");
    if (system("command -v curl >/dev/null 2>&1") != 0) {
        printf("  SKIP: curl not available\n");
        return 0;
    }
    if (system("command -v openssl >/dev/null 2>&1") != 0) {
        printf("  SKIP: openssl not available\n");
        return 0;
    }
    test_tls_ctx_helpers();
    test_tls_health_e2e();
    printf("All TLS tests PASSED\n");
    return 0;
}
