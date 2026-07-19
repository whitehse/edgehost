/**
 * P1.4c: /health JSON + metrics; routing and parse errors.
 */
#include "edge_config.h"
#include "edge_iouring.h"
#include "edge_metrics.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static uint16_t pick_port(void)
{
    return (uint16_t)(19200 + (getpid() % 800));
}

static int client_exchange(uint16_t port, const char *req, char *out,
                           size_t out_sz)
{
    int fd;
    struct sockaddr_in addr;
    ssize_t n;
    size_t total = 0;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (write(fd, req, strlen(req)) < 0) {
        close(fd);
        return -1;
    }
    while (total + 1 < out_sz) {
        n = read(fd, out + total, out_sz - 1 - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
    }
    out[total] = '\0';
    close(fd);
    return (int)total;
}

static int start_server(uint16_t port, int max_accepts, pid_t *out_pid)
{
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        edge_config_t cfg;
        edge_iouring_opts_t opts;

        edge_config_defaults(&cfg);
        snprintf(cfg.listen_host, sizeof(cfg.listen_host), "127.0.0.1");
        cfg.listen_port = port;
        /* Unit tests do not assume SPA fixtures on cwd; clear docroots. */
        cfg.spa_root[0] = '\0';
        cfg.packages_root[0] = '\0';
        edge_iouring_opts_defaults(&opts);
        opts.max_accepts = max_accepts;
        _exit(edge_iouring_run(&cfg, &opts) == 0 ? 0 : 1);
    }
    *out_pid = pid;
    return 0;
}

static int wait_ready(uint16_t port, const char *req, char *buf, size_t buflen)
{
    int attempt;
    int n = -1;
    for (attempt = 0; attempt < 50; attempt++) {
        usleep(20 * 1000);
        n = client_exchange(port, req, buf, buflen);
        if (n > 0) {
            return n;
        }
    }
    return n;
}

static void test_metrics_json_unit(void)
{
    edge_metrics_t m;
    char buf[256];
    int n;

    edge_metrics_init(&m);
    m.accepts = 3;
    m.requests = 2;
    m.responses_2xx = 2;
    n = edge_metrics_format_health_json(&m, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"status\":\"ok\"") != NULL);
    assert(strstr(buf, "\"accepts\":3") != NULL);
    assert(strstr(buf, "\"requests\":2") != NULL);
    printf("  PASS: metrics JSON unit format\n");
}

static int test_health_json(void)
{
    uint16_t port = pick_port();
    pid_t pid;
    int status;
    char buf[2048];
    int n;
    const char *req =
        "GET /health HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    if (start_server(port, 1, &pid) != 0) {
        return 1;
    }
    n = wait_ready(port, req, buf, sizeof(buf));
    if (n <= 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        fprintf(stderr, "health: no response\n");
        return 1;
    }
    assert(strstr(buf, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(buf, "application/json") != NULL);
    assert(strstr(buf, "\"status\":\"ok\"") != NULL);
    assert(strstr(buf, "\"accepts\":") != NULL);
    assert(strstr(buf, "\"requests\":") != NULL);
    assert(strstr(buf, "\"uptime_s\":") != NULL);
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "health: child status %d\n", status);
        return 1;
    }
    printf("  PASS: GET /health → JSON metrics\n");
    return 0;
}

static int test_root_ok(void)
{
    uint16_t port = (uint16_t)(pick_port() + 2);
    pid_t pid;
    int status;
    char buf[1024];
    int n;
    const char *req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    if (start_server(port, 1, &pid) != 0) {
        return 1;
    }
    n = wait_ready(port, req, buf, sizeof(buf));
    if (n <= 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        return 1;
    }
    assert(strstr(buf, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(buf, "text/plain") != NULL);
    assert(strstr(buf, "ok") != NULL);
    waitpid(pid, &status, 0);
    printf("  PASS: GET / → plain ok\n");
    return 0;
}

static int test_not_found(void)
{
    uint16_t port = (uint16_t)(pick_port() + 4);
    pid_t pid;
    int status;
    char buf[1024];
    int n;
    const char *req =
        "GET /nope HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    if (start_server(port, 1, &pid) != 0) {
        return 1;
    }
    n = wait_ready(port, req, buf, sizeof(buf));
    if (n <= 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        return 1;
    }
    assert(strstr(buf, "HTTP/1.1 404") != NULL);
    waitpid(pid, &status, 0);
    printf("  PASS: GET /nope → 404\n");
    return 0;
}

static int test_malformed(void)
{
    uint16_t port = (uint16_t)(pick_port() + 6);
    pid_t pid;
    int status;
    char buf[1024];
    int n;
    const char *req = "GET\r\n\r\n";

    if (start_server(port, 1, &pid) != 0) {
        return 1;
    }
    n = wait_ready(port, req, buf, sizeof(buf));
    if (n <= 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        return 1;
    }
    assert(strstr(buf, "HTTP/1.1 400") != NULL);
    waitpid(pid, &status, 0);
    printf("  PASS: malformed → 400\n");
    return 0;
}

static int test_partial_root(void)
{
    uint16_t port = (uint16_t)(pick_port() + 8);
    pid_t pid;
    int status;
    int fd;
    struct sockaddr_in addr;
    char buf[1024];
    size_t total = 0;
    ssize_t nr;
    int attempt;

    if (start_server(port, 1, &pid) != 0) {
        return 1;
    }

    for (attempt = 0; attempt < 50; attempt++) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return 1;
        }
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            break;
        }
        close(fd);
        fd = -1;
        usleep(20 * 1000);
    }
    if (fd < 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        return 1;
    }

    (void)write(fd, "GET / HTTP/1.1\r\n", 16);
    usleep(5 * 1000);
    (void)write(fd, "Host: t\r\n\r\n", 11);

    while (total + 1 < sizeof(buf)) {
        nr = read(fd, buf + total, sizeof(buf) - 1 - total);
        if (nr <= 0) {
            break;
        }
        total += (size_t)nr;
    }
    buf[total] = '\0';
    close(fd);

    assert(strstr(buf, "HTTP/1.1 200 OK") != NULL);
    waitpid(pid, &status, 0);
    printf("  PASS: partial feed GET / → 200\n");
    return 0;
}

int main(void)
{
    printf("iouring_health:\n");
    test_metrics_json_unit();
    if (test_health_json() != 0) {
        return 1;
    }
    if (test_root_ok() != 0) {
        return 1;
    }
    if (test_not_found() != 0) {
        return 1;
    }
    if (test_malformed() != 0) {
        return 1;
    }
    if (test_partial_root() != 0) {
        return 1;
    }
    printf("all passed\n");
    return 0;
}
