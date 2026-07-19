/**
 * @file iouring_loop.c
 * @brief io_uring accept + shared edge_http1_serve (P1.4c / P1.5).
 */

#define _GNU_SOURCE

#include "edge_iouring.h"

#include "edge_http1_serve.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <liburing.h>

enum {
    OP_ACCEPT = 1,
    OP_RECV   = 2,
    OP_SEND   = 3
};

typedef enum {
    CS_FREE = 0,
    CS_RECV,
    CS_SEND
} conn_state_t;

typedef struct {
    int                  fd;
    conn_state_t         state;
    int                  slot;
    int                  recv_pending;
    int                  send_pending;
    uint8_t              recv_buf[4096];
    char                 send_buf[2048];
    size_t               send_len;
    size_t               send_off;
    edge_http1_serve_t  *http;
} conn_t;

typedef struct {
    const edge_config_t       *cfg;
    const edge_iouring_opts_t *opts;
    struct io_uring            ring;
    int                        listen_fd;
    conn_t                    *conns;
    int                        max_conns;
    struct sockaddr_storage    client_addr;
    socklen_t                  client_addr_len;
    int                        accepts_done;
    int                        accept_pending;
    edge_metrics_t            *metrics;
    edge_metrics_t             metrics_local;
} server_t;

void edge_iouring_opts_defaults(edge_iouring_opts_t *o)
{
    if (!o) {
        return;
    }
    memset(o, 0, sizeof(*o));
    o->ring_entries = 256;
    o->max_conns = 64;
    o->backlog = 128;
    o->max_accepts = 0;
    o->stop = NULL;
    o->static_body = NULL;
    o->static_body_len = 0;
    o->metrics = NULL;
}

static uint64_t pack_ud(int op, int slot)
{
    return ((uint64_t)(uint32_t)op << 32) | (uint32_t)slot;
}

static void unpack_ud(uint64_t ud, int *op, int *slot)
{
    *op = (int)(ud >> 32);
    *slot = (int)(ud & 0xffffffffu);
}

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int create_listen_socket(const edge_config_t *cfg, int backlog)
{
    int fd;
    int on = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("edgehost: socket");
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) {
        perror("edgehost: setsockopt REUSEADDR");
        close(fd);
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->listen_port);
    if (cfg->listen_host[0] == '\0' ||
        strcmp(cfg->listen_host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, cfg->listen_host, &addr.sin_addr) != 1) {
        fprintf(stderr, "edgehost: invalid listen host %s\n", cfg->listen_host);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("edgehost: bind");
        close(fd);
        return -1;
    }
    if (listen(fd, backlog > 0 ? backlog : 128) != 0) {
        perror("edgehost: listen");
        close(fd);
        return -1;
    }
    if (set_nonblock(fd) != 0) {
        perror("edgehost: listen nonblock");
        close(fd);
        return -1;
    }
    return fd;
}

static void close_conn(server_t *srv, conn_t *c)
{
    if (!c || c->state == CS_FREE) {
        return;
    }
    if (c->http) {
        edge_http1_serve_destroy(c->http);
        c->http = NULL;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
        if (srv && srv->metrics && srv->metrics->active_conns > 0) {
            srv->metrics->active_conns--;
        }
    }
    c->state = CS_FREE;
    c->recv_pending = c->send_pending = 0;
    c->send_len = c->send_off = 0;
}

static conn_t *alloc_conn(server_t *srv)
{
    int i;
    for (i = 0; i < srv->max_conns; i++) {
        if (srv->conns[i].state == CS_FREE) {
            conn_t *c = &srv->conns[i];
            memset(c, 0, sizeof(*c));
            c->slot = i;
            c->fd = -1;
            c->state = CS_RECV;
            c->http = edge_http1_serve_create();
            if (!c->http) {
                return NULL;
            }
            return c;
        }
    }
    return NULL;
}

static int submit_accept(server_t *srv)
{
    struct io_uring_sqe *sqe;

    if (srv->accept_pending) {
        return 0;
    }
    if (srv->opts->max_accepts > 0 &&
        srv->accepts_done >= srv->opts->max_accepts) {
        return 0;
    }
    sqe = io_uring_get_sqe(&srv->ring);
    if (!sqe) {
        return -1;
    }
    srv->client_addr_len = sizeof(srv->client_addr);
    io_uring_prep_accept(sqe, srv->listen_fd,
                         (struct sockaddr *)&srv->client_addr,
                         &srv->client_addr_len, SOCK_CLOEXEC);
    io_uring_sqe_set_data64(sqe, pack_ud(OP_ACCEPT, 0));
    srv->accept_pending = 1;
    return 0;
}

static int submit_recv(server_t *srv, conn_t *c)
{
    struct io_uring_sqe *sqe;

    if (c->recv_pending || c->state != CS_RECV) {
        return 0;
    }
    sqe = io_uring_get_sqe(&srv->ring);
    if (!sqe) {
        return -1;
    }
    io_uring_prep_recv(sqe, c->fd, c->recv_buf, sizeof(c->recv_buf), 0);
    io_uring_sqe_set_data64(sqe, pack_ud(OP_RECV, c->slot));
    c->recv_pending = 1;
    return 0;
}

static int submit_send(server_t *srv, conn_t *c)
{
    struct io_uring_sqe *sqe;
    size_t n;

    if (c->send_pending || c->send_off >= c->send_len) {
        return 0;
    }
    n = c->send_len - c->send_off;
    sqe = io_uring_get_sqe(&srv->ring);
    if (!sqe) {
        return -1;
    }
    io_uring_prep_send(sqe, c->fd, c->send_buf + c->send_off, n, 0);
    io_uring_sqe_set_data64(sqe, pack_ud(OP_SEND, c->slot));
    c->send_pending = 1;
    return 0;
}

static int begin_send_buf(server_t *srv, conn_t *c, size_t len)
{
    c->send_len = len;
    c->send_off = 0;
    c->state = CS_SEND;
    return submit_send(srv, c);
}

static int begin_bad_request_plain(server_t *srv, conn_t *c)
{
    static const char resp[] =
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 12\r\n"
        "Connection: close\r\n"
        "\r\n"
        "bad request\n";
    if (sizeof(resp) - 1 >= sizeof(c->send_buf)) {
        return -1;
    }
    memcpy(c->send_buf, resp, sizeof(resp) - 1);
    if (srv->metrics) {
        srv->metrics->requests++;
        srv->metrics->responses_4xx++;
    }
    return begin_send_buf(srv, c, sizeof(resp) - 1);
}

static int accept_one(server_t *srv, int client_fd)
{
    conn_t *c;

    if (client_fd < 0) {
        return -1;
    }
    if (set_nonblock(client_fd) != 0) {
        close(client_fd);
        return -1;
    }
    c = alloc_conn(srv);
    if (!c) {
        close(client_fd);
        if (srv->metrics) {
            srv->metrics->rejects++;
        }
        return -1;
    }
    c->fd = client_fd;
    srv->accepts_done++;
    if (srv->metrics) {
        srv->metrics->accepts++;
        srv->metrics->active_conns++;
    }
    if (submit_recv(srv, c) != 0) {
        close_conn(srv, c);
        return -1;
    }
    return 0;
}

static int should_stop(const server_t *srv)
{
    if (srv->opts->stop && *srv->opts->stop) {
        return 1;
    }
    if (srv->opts->max_accepts > 0 &&
        srv->accepts_done >= srv->opts->max_accepts) {
        int i;
        if (srv->accept_pending) {
            return 0;
        }
        for (i = 0; i < srv->max_conns; i++) {
            if (srv->conns[i].state != CS_FREE) {
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

int edge_iouring_run(const edge_config_t *cfg, const edge_iouring_opts_t *opts)
{
    edge_iouring_opts_t local;
    server_t srv;
    int rc;
    int i;
    int exit_code = 0;

    if (!cfg) {
        return 1;
    }
    if (!opts) {
        edge_iouring_opts_defaults(&local);
        opts = &local;
    }

    memset(&srv, 0, sizeof(srv));
    srv.cfg = cfg;
    srv.opts = opts;
    srv.listen_fd = -1;
    srv.max_conns = opts->max_conns > 0 ? opts->max_conns : 64;
    if (opts->metrics) {
        srv.metrics = opts->metrics;
        if (srv.metrics->started_at == 0) {
            edge_metrics_init(srv.metrics);
        }
    } else {
        edge_metrics_init(&srv.metrics_local);
        srv.metrics = &srv.metrics_local;
    }

    srv.conns = (conn_t *)calloc((size_t)srv.max_conns, sizeof(conn_t));
    if (!srv.conns) {
        return 1;
    }
    for (i = 0; i < srv.max_conns; i++) {
        srv.conns[i].fd = -1;
        srv.conns[i].state = CS_FREE;
        srv.conns[i].slot = i;
    }

    srv.listen_fd = create_listen_socket(cfg, opts->backlog);
    if (srv.listen_fd < 0) {
        free(srv.conns);
        return 1;
    }

    {
        int entries = opts->ring_entries > 0 ? opts->ring_entries : 256;
        rc = io_uring_queue_init(entries, &srv.ring, 0);
        if (rc < 0) {
            fprintf(stderr, "edgehost: io_uring_queue_init: %s\n",
                    strerror(-rc));
            close(srv.listen_fd);
            free(srv.conns);
            return 1;
        }
    }

    if (submit_accept(&srv) != 0) {
        io_uring_queue_exit(&srv.ring);
        close(srv.listen_fd);
        free(srv.conns);
        return 1;
    }
    io_uring_submit(&srv.ring);

    fprintf(stderr,
            "edgehost: listening on %s:%u (HTTP/1 + /health)\n",
            cfg->listen_host[0] ? cfg->listen_host : "0.0.0.0",
            (unsigned)cfg->listen_port);

    while (!should_stop(&srv)) {
        struct io_uring_cqe *cqe = NULL;
        struct __kernel_timespec ts;
        int op, slot;
        int res;

        ts.tv_sec = 0;
        ts.tv_nsec = 200 * 1000 * 1000L;
        rc = io_uring_wait_cqe_timeout(&srv.ring, &cqe, &ts);
        if (rc == -ETIME || rc == -EAGAIN) {
            if (should_stop(&srv)) {
                break;
            }
            continue;
        }
        if (rc < 0) {
            if (rc == -EINTR) {
                continue;
            }
            fprintf(stderr, "edgehost: wait_cqe: %s\n", strerror(-rc));
            exit_code = 2;
            break;
        }

        res = cqe->res;
        unpack_ud(cqe->user_data, &op, &slot);
        io_uring_cqe_seen(&srv.ring, cqe);

        if (op == OP_ACCEPT) {
            srv.accept_pending = 0;
            if (res >= 0) {
                (void)accept_one(&srv, res);
            } else if (res != -EAGAIN && res != -ECONNABORTED &&
                       res != -EINTR) {
                fprintf(stderr, "edgehost: accept: %s\n", strerror(-res));
            }
            (void)submit_accept(&srv);
        } else if (op == OP_RECV) {
            conn_t *c = (slot >= 0 && slot < srv.max_conns)
                            ? &srv.conns[slot]
                            : NULL;
            if (c && c->state == CS_RECV) {
                size_t rlen = 0;
                int pr;
                c->recv_pending = 0;
                if (res < 0) {
                    close_conn(&srv, c);
                } else if (res == 0) {
                    if (begin_bad_request_plain(&srv, c) != 0) {
                        close_conn(&srv, c);
                    }
                } else {
                    pr = edge_http1_serve_feed(c->http, c->recv_buf,
                                               (size_t)res, srv.metrics,
                                               c->send_buf, sizeof(c->send_buf),
                                               &rlen);
                    if (pr < 0) {
                        close_conn(&srv, c);
                    } else if (pr == 0) {
                        if (submit_recv(&srv, c) != 0) {
                            close_conn(&srv, c);
                        }
                    } else {
                        if (begin_send_buf(&srv, c, rlen) != 0) {
                            close_conn(&srv, c);
                        }
                    }
                }
            }
        } else if (op == OP_SEND) {
            conn_t *c = (slot >= 0 && slot < srv.max_conns)
                            ? &srv.conns[slot]
                            : NULL;
            if (c && c->state != CS_FREE) {
                c->send_pending = 0;
                if (res < 0) {
                    close_conn(&srv, c);
                } else {
                    if (srv.metrics && res > 0) {
                        srv.metrics->bytes_out += (uint64_t)res;
                    }
                    c->send_off += (size_t)res;
                    if (c->send_off >= c->send_len) {
                        close_conn(&srv, c);
                    } else if (submit_send(&srv, c) != 0) {
                        close_conn(&srv, c);
                    }
                }
            }
        }

        io_uring_submit(&srv.ring);
    }

    for (i = 0; i < srv.max_conns; i++) {
        close_conn(&srv, &srv.conns[i]);
    }
    io_uring_queue_exit(&srv.ring);
    close(srv.listen_fd);
    free(srv.conns);
    fprintf(stderr, "edgehost: iouring loop exit (%d accepts)\n",
            srv.accepts_done);
    return exit_code;
}
