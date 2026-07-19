/**
 * P1.4a: fork edge_iouring_run, connect, expect fixed static response.
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
    /* High ephemeral-ish range; conflict → retry once in main. */
    return (uint16_t)(19000 + (getpid() % 1000));
}

static int client_exchange(uint16_t port, char *out, size_t out_sz)
{
    int fd;
    struct sockaddr_in addr;
    const char *req = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
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

static int run_once(uint16_t port)
{
    pid_t pid;
    int status;
    char buf[1024];
    int n;
    int attempt;

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        edge_config_t cfg;
        edge_iouring_opts_t opts;

        edge_config_defaults(&cfg);
        snprintf(cfg.listen_host, sizeof(cfg.listen_host), "127.0.0.1");
        cfg.listen_port = port;
        edge_iouring_opts_defaults(&opts);
        opts.max_accepts = 1;
        _exit(edge_iouring_run(&cfg, &opts) == 0 ? 0 : 1);
    }

    /* Wait for listen */
    for (attempt = 0; attempt < 50; attempt++) {
        usleep(20 * 1000);
        n = client_exchange(port, buf, sizeof(buf));
        if (n > 0) {
            break;
        }
    }
    if (n <= 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        fprintf(stderr, "client_exchange failed\n");
        return 1;
    }

    assert(strstr(buf, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(buf, "ok") != NULL);
    printf("  response (%d bytes): first line contains 200 OK\n", n);

    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "child failed status=%d\n", status);
        return 1;
    }
    return 0;
}

int main(void)
{
    uint16_t port = pick_port();
    int rc;

    printf("iouring_static:\n");
    rc = run_once(port);
    if (rc != 0) {
        /* port busy — try another */
        port = (uint16_t)(port + 17);
        rc = run_once(port);
    }
    if (rc != 0) {
        return 1;
    }
    printf("  PASS: accept + fixed static response\n");
    printf("all passed\n");
    return 0;
}
