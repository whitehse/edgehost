/**
 * P1.4b: shaggy HTTP/1 bridge — valid GET → 200; malformed → 400.
 */
#include "edge_config.h"
#include "edge_iouring.h"

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
    return (uint16_t)(19100 + (getpid() % 900));
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

static int test_valid_get(void)
{
    uint16_t port = pick_port();
    pid_t pid;
    int status;
    char buf[1024];
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
        fprintf(stderr, "valid GET: no response\n");
        return 1;
    }
    assert(strstr(buf, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(buf, "ok") != NULL);
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "valid GET: child status %d\n", status);
        return 1;
    }
    printf("  PASS: valid GET → 200 via shaggy parse\n");
    return 0;
}

static int test_malformed(void)
{
    uint16_t port = (uint16_t)(pick_port() + 3);
    pid_t pid;
    int status;
    char buf[1024];
    int n;
    /* Missing version / spaces — shaggy emits PROTOCOL_EVENT_ERROR */
    const char *req = "GET\r\n\r\n";

    if (start_server(port, 1, &pid) != 0) {
        return 1;
    }
    n = wait_ready(port, req, buf, sizeof(buf));
    if (n <= 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        fprintf(stderr, "malformed: no response\n");
        return 1;
    }
    assert(strstr(buf, "HTTP/1.1 400") != NULL);
    waitpid(pid, &status, 0);
    printf("  PASS: malformed → 400\n");
    return 0;
}

static int test_partial_then_complete(void)
{
    uint16_t port = (uint16_t)(pick_port() + 7);
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

    /* Wait for listen */
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
        fprintf(stderr, "partial: connect failed\n");
        return 1;
    }

    /* Split request across writes */
    (void)write(fd, "GET /x HTTP/1.1\r\n", 17);
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
    printf("  PASS: partial feed → 200\n");
    return 0;
}

int main(void)
{
    printf("iouring_http1:\n");
    if (test_valid_get() != 0) {
        return 1;
    }
    if (test_malformed() != 0) {
        return 1;
    }
    if (test_partial_then_complete() != 0) {
        return 1;
    }
    printf("all passed\n");
    return 0;
}
