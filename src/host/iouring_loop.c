/**
 * @file iouring_loop.c
 * @brief P1.4c: io_uring + shaggy HTTP/1 + GET /health JSON + metrics.
 */

#define _GNU_SOURCE

#include "edge_iouring.h"

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

#include "http1.h"
#include "protocol_events.h"

enum {
    OP_ACCEPT = 1,
    OP_RECV   = 2,
    OP_SEND   = 3
};

typedef enum {
    CS_FREE = 0,
    CS_RECV,
    CS_SEND,
    CS_CLOSING
} conn_state_t;

typedef struct {
    int           fd;
    conn_state_t  state;
    int           slot;
    int           recv_pending;
    int           send_pending;
    uint8_t       recv_buf[4096];
    char          send_buf[2048];
    size_t        send_len;
    size_t        send_off;
    http1_ctx_t  *h1;
    char          method[16];
    char          path[256];
    int           headers_done;
    int           status_class; /* 2 or 4 once response chosen */
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
    const char                *static_body;
    size_t                     static_body_len;
    edge_metrics_t            *metrics;
    edge_metrics_t             metrics_local;
} server_t;

static const char k_default_body[] = "ok\n";

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
    if (c->h1) {
        http1_destroy(c->h1);
        c->h1 = NULL;
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
    c->headers_done = 0;
    c->status_class = 0;
    c->method[0] = c->path[0] = '\0';
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
            c->h1 = http1_create(HTTP1_ROLE_SERVER);
            if (!c->h1) {
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

static int begin_response(server_t *srv, conn_t *c, int status,
                          const char *reason, const char *ctype,
                          const char *body, size_t body_len)
{
    int n;

    if (!reason) {
        reason = "OK";
    }
    if (!ctype) {
        ctype = "text/plain";
    }
    if (!body) {
        body = "";
        body_len = 0;
    }

    n = snprintf(c->send_buf, sizeof(c->send_buf),
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 status, reason, ctype, body_len);
    if (n < 0 || (size_t)n >= sizeof(c->send_buf)) {
        return -1;
    }
    if (body_len > 0) {
        if ((size_t)n + body_len >= sizeof(c->send_buf)) {
            return -1;
        }
        memcpy(c->send_buf + n, body, body_len);
        n += (int)body_len;
    }
    c->send_len = (size_t)n;
    c->send_off = 0;
    c->state = CS_SEND;
    c->status_class = (status >= 200 && status < 300) ? 2 : 4;

    if (srv->metrics) {
        srv->metrics->requests++;
        if (c->status_class == 2) {
            srv->metrics->responses_2xx++;
        } else {
            srv->metrics->responses_4xx++;
        }
    }
    return submit_send(srv, c);
}

static int path_is_health(const char *path)
{
    if (!path) {
        return 0;
    }
    /* exact /health or /health?… (shaggy path usually has no query) */
    if (strcmp(path, "/health") == 0) {
        return 1;
    }
    if (strncmp(path, "/health?", 8) == 0) {
        return 1;
    }
    return 0;
}

static int begin_health(server_t *srv, conn_t *c)
{
    char json[512];
    int jn;

    jn = edge_metrics_format_health_json(srv->metrics, json, sizeof(json));
    if (jn < 0) {
        return begin_response(srv, c, 500, "Internal Server Error",
                              "text/plain", "metrics error\n", 14);
    }
    return begin_response(srv, c, 200, "OK", "application/json", json,
                          (size_t)jn);
}

static int begin_ok_plain(server_t *srv, conn_t *c)
{
    return begin_response(srv, c, 200, "OK", "text/plain", srv->static_body,
                          srv->static_body_len);
}

static int begin_bad_request(server_t *srv, conn_t *c)
{
    static const char body[] = "bad request\n";
    return begin_response(srv, c, 400, "Bad Request", "text/plain", body,
                          sizeof(body) - 1);
}

static int begin_not_found(server_t *srv, conn_t *c)
{
    static const char body[] = "not found\n";
    return begin_response(srv, c, 404, "Not Found", "text/plain", body,
                          sizeof(body) - 1);
}

static int dispatch_request(server_t *srv, conn_t *c)
{
    /* Only GET is reliably parsed by current shaggy server path. */
    if (c->method[0] && strcmp(c->method, "GET") != 0) {
        static const char body[] = "method not allowed\n";
        return begin_response(srv, c, 405, "Method Not Allowed", "text/plain",
                              body, sizeof(body) - 1);
    }
    if (path_is_health(c->path)) {
        return begin_health(srv, c);
    }
    /* Root stays friendly plain ok; other paths 404 so /health is distinct. */
    if (c->path[0] == '\0' || strcmp(c->path, "/") == 0) {
        return begin_ok_plain(srv, c);
    }
    return begin_not_found(srv, c);
}

/**
 * Feed bytes into shaggy and act on events.
 * @return 1 response started, 0 need more data, -1 close
 */
static int on_http1_bytes(server_t *srv, conn_t *c, const uint8_t *data,
                          size_t len)
{
    protocol_event_t ev;
    int respond = 0;
    int bad = 0;

    if (!c->h1 || len == 0) {
        return -1;
    }

    if (srv->metrics) {
        srv->metrics->bytes_in += (uint64_t)len;
    }

    (void)http1_feed_input(c->h1, data, len);

    while (http1_next_event(c->h1, &ev)) {
        if (ev.type == PROTOCOL_EVENT_ERROR) {
            bad = 1;
            break;
        }
        if (ev.type == HTTP1_EVENT_REQUEST_LINE) {
            snprintf(c->method, sizeof(c->method), "%s",
                     ev.u.http1_request.method);
            snprintf(c->path, sizeof(c->path), "%s",
                     ev.u.http1_request.path);
        } else if (ev.type == HTTP1_EVENT_HEADERS_COMPLETE) {
            c->headers_done = 1;
            respond = 1;
        }
    }

    if (bad) {
        if (begin_bad_request(srv, c) != 0) {
            return -1;
        }
        return 1;
    }
    if (respond) {
        if (dispatch_request(srv, c) != 0) {
            return -1;
        }
        return 1;
    }
    return 0;
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
    if (opts->static_body && opts->static_body_len > 0) {
        srv.static_body = opts->static_body;
        srv.static_body_len = opts->static_body_len;
    } else {
        srv.static_body = k_default_body;
        srv.static_body_len = sizeof(k_default_body) - 1;
    }
    if (opts->metrics) {
        srv.metrics = opts->metrics;
        /* leave caller-owned zeroed or pre-inited; ensure started_at set */
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
            "edgehost: listening on %s:%u (HTTP/1 + /health P1.4c)\n",
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
                int pr;
                c->recv_pending = 0;
                if (res < 0) {
                    close_conn(&srv, c);
                } else if (res == 0) {
                    if (!c->headers_done) {
                        if (begin_bad_request(&srv, c) != 0) {
                            close_conn(&srv, c);
                        }
                    } else {
                        close_conn(&srv, c);
                    }
                } else {
                    pr = on_http1_bytes(&srv, c, c->recv_buf, (size_t)res);
                    if (pr < 0) {
                        close_conn(&srv, c);
                    } else if (pr == 0) {
                        if (submit_recv(&srv, c) != 0) {
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
