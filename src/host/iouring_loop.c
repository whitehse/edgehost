/**
 * @file iouring_loop.c
 * @brief io_uring accept + HTTP/1 + WS + optional OpenSSL TLS server (P1.13).
 */

#define _GNU_SOURCE

#include "edge_iouring.h"

#include "edge_auth.h"
#include "edge_config.h"
#include "edge_config_hup.h"
#include "edge_http1_serve.h"
#include "edge_pq_sidecar.h"
#include "edge_state.h"
#include "edge_tls.h"
#include "edge_ws.h"
#include "edgecore.h"
#include "edge_ca.h"
#include "edge_clickhouse.h"
#include "edge_debug.h"
#include "host_alloc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <liburing.h>
#include <openssl/ssl.h>

#include "websocket.h"
#include "protocol_events.h"

/* Domain demux in user_data high byte (design pack_ud3). HTTP domain 0 keeps
 * legacy user_data layout for op values < 2^24. */
#define UD_DOMAIN_HTTP 0u
#define UD_DOMAIN_E7   1u

enum {
    OP_ACCEPT = 1,
    OP_RECV   = 2,
    OP_SEND   = 3,
    OP_POLL   = 4
};

enum {
    E7_OP_ACCEPT = 1
    /* Session RECV/SEND remain on tick poll in PR-4a; full CQE demux later. */
};

typedef enum {
    CS_FREE = 0,
    CS_TLS_HS,   /* OpenSSL handshake (non-blocking) */
    CS_RECV,     /* HTTP request */
    CS_SEND,     /* HTTP response (close after unless ws_upgrade pending) */
    CS_WS_RECV,  /* WebSocket open, waiting for frames or fan-out */
    CS_WS_SEND   /* WebSocket sending frames */
} conn_state_t;

typedef struct {
    int                  fd;
    conn_state_t         state;
    int                  slot;
    int                  recv_pending;
    int                  send_pending;
    int                  poll_pending;
    uint8_t              recv_buf[4096];
    char                *send_buf; /* heap: large enough for SPA assets / WS */
    size_t               send_cap;
    size_t               send_len;
    size_t               send_off;
    edge_http1_serve_t  *http;
    websocket_ctx_t     *ws;       /* non-NULL after successful upgrade */
    int                  keep_ws;  /* after CS_SEND of 101, enter WS mode */
    SSL                 *ssl;      /* non-NULL => TLS via SSL_set_fd */
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
    edge_http1_docroot_t       docroots;
    size_t                     send_cap;
    edge_state_store_t        *store;
    int                        store_owned;
    edge_ws_hub_t             *hub;
    edge_auth_ctx_t           *auth; /* not owned; may be NULL */
    edge_plugin_host_t        *plugins;
    const char                *service_api_key;
    SSL_CTX                   *tls_ctx; /* NULL => plain TCP */
    edge_pq_sidecar_config_t   pq_cfg;
    uint64_t                   last_scrape_ms;
    edge_e7_callhome_t        *e7; /* optional; not owned */
    edge_clickhouse_t         *clickhouse; /* optional; not owned */
    const char                *telemetry_user;     /* from cfg; not owned */
    const char                *telemetry_password; /* from cfg; not owned */
    edge_ca_t                 *ca;         /* optional; not owned */
    int                        e7_accept_pending;
    struct sockaddr_storage    e7_client_addr;
    socklen_t                  e7_client_addr_len;
    uint64_t                   mono_ms;
    /* Durable shadow when HUP reloads without edgecore (opts->core NULL). */
    edge_config_t              hup_shadow;
    int                        hup_shadow_valid;
} server_t;

static int promote_to_ws(server_t *srv, conn_t *c);
static int ws_try_send_pending(server_t *srv, conn_t *c);
static int handle_ws_recv(server_t *srv, conn_t *c, size_t n);
static void ws_flush_all_pending(server_t *srv);
static int handle_http_bytes(server_t *srv, conn_t *c, size_t n);

static void publish_http_debug(server_t *srv)
{
    edge_debug_http_snap_t snap;
    int i;
    int active = 0;
    int ws = 0;

    if (!srv) {
        return;
    }
    memset(&snap, 0, sizeof(snap));
    snap.max_conns = srv->max_conns;
    snap.send_cap = srv->send_cap;
    snap.conn_table_bytes =
        (size_t)srv->max_conns * sizeof(conn_t);
    if (srv->metrics) {
        snap.accepts = srv->metrics->accepts;
        snap.requests = srv->metrics->requests;
        snap.active_conns = (int)srv->metrics->active_conns;
    }
    for (i = 0; i < srv->max_conns; i++) {
        if (srv->conns[i].state == CS_FREE) {
            continue;
        }
        active++;
        if (srv->conns[i].state == CS_WS_RECV ||
            srv->conns[i].state == CS_WS_SEND || srv->conns[i].keep_ws) {
            ws++;
        }
    }
    snap.active_conns = active;
    snap.ws_conns = ws;
    snap.send_bufs_live = (size_t)active * srv->send_cap;
    snap.recv_bufs_embedded = (size_t)active * sizeof(((conn_t *)0)->recv_buf);
    snap.valid = 1;
    edge_debug_http_publish(&snap);
}

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
    o->state = NULL;
    o->auth = NULL;
    o->plugins = NULL;
    o->service_api_key = NULL;
    o->e7 = NULL;
    o->clickhouse = NULL;
    o->ca = NULL;
    o->config_path = NULL;
    o->core = NULL;
}

/**
 * SIGHUP: reload YAML, apply edgecore (if any), state ns flags, e7 allowlist.
 * Listen sockets are not rebound (log inside e7_callhome_apply_config).
 */
static void try_hup_reload(server_t *srv)
{
    const char *path;
    char err[160];
    const edge_config_t *applied = NULL;

    if (!srv || !srv->opts || !edgehost_hup_take()) {
        return;
    }
    path = srv->opts->config_path;
    if (!path || !path[0]) {
        fprintf(stderr,
                "edgehost: SIGHUP ignored (no config_path on iouring opts)\n");
        return;
    }

    if (srv->opts->core) {
        if (edgehost_reload_config(srv->opts->core, path, err, sizeof(err)) !=
            0) {
            fprintf(stderr, "edgehost: SIGHUP reload failed: %s\n", err);
            return;
        }
        applied = edgecore_config(srv->opts->core);
        fprintf(stderr, "edgehost: SIGHUP config applied gen=%llu\n",
                applied ? (unsigned long long)applied->generation : 0ull);
    } else {
        if (edge_config_load_yaml_path(path, &srv->hup_shadow, err,
                                       sizeof(err)) != 0) {
            fprintf(stderr, "edgehost: SIGHUP load failed: %s\n", err);
            return;
        }
        if (edge_config_validate(&srv->hup_shadow, err, sizeof(err)) != 0) {
            fprintf(stderr, "edgehost: SIGHUP validate failed: %s\n", err);
            return;
        }
        srv->hup_shadow_valid = 1;
        applied = &srv->hup_shadow;
        fprintf(stderr,
                "edgehost: SIGHUP config loaded (no edgecore; state/e7 only)\n");
    }

    if (!applied) {
        return;
    }
    if (srv->store) {
        edge_state_apply_config(srv->store, applied);
    }
    if (srv->e7) {
        (void)edge_e7_callhome_apply_config(srv->e7, applied);
    }
    /* Keep srv.cfg pointing at live applied config. */
    srv->cfg = applied;
}

static uint64_t pack_ud3(uint8_t domain, uint32_t op, uint32_t slot)
{
    return ((uint64_t)domain << 56) | ((uint64_t)(op & 0xffffffu) << 32) |
           (uint64_t)slot;
}

static void unpack_ud3(uint64_t ud, uint8_t *domain, uint32_t *op,
                       uint32_t *slot)
{
    *domain = (uint8_t)(ud >> 56);
    *op = (uint32_t)((ud >> 32) & 0xffffffu);
    *slot = (uint32_t)(ud & 0xffffffffu);
}

/* HTTP-domain convenience (domain 0). */
static uint64_t pack_ud(int op, int slot)
{
    return pack_ud3(UD_DOMAIN_HTTP, (uint32_t)op, (uint32_t)slot);
}

static uint64_t mono_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
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
    if (srv && srv->hub) {
        edge_ws_hub_unsubscribe(srv->hub, c->slot);
    }
    if (c->ws) {
        websocket_destroy(c->ws);
        c->ws = NULL;
    }
    if (c->http) {
        edge_http1_serve_destroy(c->http);
        c->http = NULL;
    }
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    host_free(c->send_buf);
    c->send_buf = NULL;
    c->send_cap = 0;
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
        if (srv && srv->metrics && srv->metrics->active_conns > 0) {
            srv->metrics->active_conns--;
        }
    }
    c->state = CS_FREE;
    c->recv_pending = c->send_pending = c->poll_pending = 0;
    c->send_len = c->send_off = 0;
    c->keep_ws = 0;
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
            c->send_cap = srv->send_cap;
            c->send_buf = (char *)host_alloc_kind(EDGE_MEM_HTTP, c->send_cap);
            if (!c->send_buf) {
                return NULL;
            }
            c->http = edge_http1_serve_create();
            if (!c->http) {
                host_free(c->send_buf);
                c->send_buf = NULL;
                return NULL;
            }
            edge_http1_serve_set_docroots(c->http, &srv->docroots);
            edge_http1_serve_set_state(c->http, srv->store);
            edge_http1_serve_set_ws_hub(c->http, srv->hub);
            edge_http1_serve_set_auth(c->http, srv->auth);
            edge_http1_serve_set_plugin_host(c->http, srv->plugins);
            edge_http1_serve_set_e7(c->http, srv->e7);
            edge_http1_serve_set_clickhouse(c->http, srv->clickhouse);
            edge_http1_serve_set_telemetry_auth(c->http, srv->telemetry_user,
                                                srv->telemetry_password);
            edge_http1_serve_set_ca(c->http, srv->ca);
            edge_http1_serve_set_outbound_policy(
                c->http, srv->cfg->dns_allow_blocking,
                srv->cfg->http_max_upstream_body_bytes);
            edge_http1_serve_set_service_api_key(c->http, srv->service_api_key);
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
    io_uring_sqe_set_data64(sqe, pack_ud3(UD_DOMAIN_HTTP, OP_ACCEPT, 0));
    srv->accept_pending = 1;
    return 0;
}

static int submit_e7_accept(server_t *srv)
{
    struct io_uring_sqe *sqe;
    int e7_fd;

    if (!srv->e7 || srv->e7_accept_pending) {
        return 0;
    }
    e7_fd = edge_e7_callhome_listen_fd(srv->e7);
    if (e7_fd < 0) {
        return 0;
    }
    sqe = io_uring_get_sqe(&srv->ring);
    if (!sqe) {
        return -1;
    }
    srv->e7_client_addr_len = sizeof(srv->e7_client_addr);
    io_uring_prep_accept(sqe, e7_fd, (struct sockaddr *)&srv->e7_client_addr,
                         &srv->e7_client_addr_len, SOCK_CLOEXEC);
    io_uring_sqe_set_data64(sqe, pack_ud3(UD_DOMAIN_E7, E7_OP_ACCEPT, 0));
    srv->e7_accept_pending = 1;
    return 0;
}

static int submit_poll(server_t *srv, conn_t *c, int want_write)
{
    struct io_uring_sqe *sqe;
    short mask;

    if (c->poll_pending) {
        return 0;
    }
    sqe = io_uring_get_sqe(&srv->ring);
    if (!sqe) {
        return -1;
    }
    mask = want_write ? (short)POLLOUT : (short)POLLIN;
    io_uring_prep_poll_add(sqe, c->fd, mask);
    io_uring_sqe_set_data64(sqe, pack_ud(OP_POLL, c->slot));
    c->poll_pending = 1;
    return 0;
}

static int submit_recv(server_t *srv, conn_t *c)
{
    struct io_uring_sqe *sqe;

    if (c->recv_pending || c->poll_pending ||
        (c->state != CS_RECV && c->state != CS_WS_RECV &&
         c->state != CS_TLS_HS)) {
        return 0;
    }
    /* TLS: readiness via POLL then SSL_read (not raw recv). */
    if (c->ssl) {
        return submit_poll(srv, c, 0);
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

static int handle_send_complete(server_t *srv, conn_t *c);

static int tls_drive_write(server_t *srv, conn_t *c)
{
    int want_w = 0;
    int n;

    if (!c->ssl || c->send_off >= c->send_len) {
        return 0;
    }
    n = edge_tls_write(c->ssl, c->send_buf + c->send_off,
                       c->send_len - c->send_off, &want_w);
    if (n > 0) {
        if (srv->metrics) {
            srv->metrics->bytes_out += (uint64_t)n;
        }
        c->send_off += (size_t)n;
        if (c->send_off >= c->send_len) {
            return handle_send_complete(srv, c);
        }
        return tls_drive_write(srv, c);
    }
    if (n == 0) {
        return submit_poll(srv, c, want_w);
    }
    return -1;
}

static int submit_send(server_t *srv, conn_t *c)
{
    struct io_uring_sqe *sqe;
    size_t n;

    if (c->send_pending || c->poll_pending || c->send_off >= c->send_len) {
        return 0;
    }
    if (c->ssl) {
        return tls_drive_write(srv, c);
    }
    n = c->send_len - c->send_off;
    sqe = io_uring_get_sqe(&srv->ring);
    if (!sqe) {
        return -1;
    }
    io_uring_prep_send(sqe, c->fd, (const void *)(c->send_buf + c->send_off), n,
                       0);
    io_uring_sqe_set_data64(sqe, pack_ud(OP_SEND, c->slot));
    c->send_pending = 1;
    return 0;
}

static int begin_send_buf(server_t *srv, conn_t *c, size_t len, conn_state_t st)
{
    c->send_len = len;
    c->send_off = 0;
    c->state = st;
    return submit_send(srv, c);
}

static int handle_send_complete(server_t *srv, conn_t *c)
{
    if (c->state == CS_SEND && c->keep_ws) {
        if (promote_to_ws(srv, c) != 0) {
            return -1;
        }
        return 0;
    }
    if (c->state == CS_WS_SEND) {
        c->state = CS_WS_RECV;
        if (ws_try_send_pending(srv, c) == 0) {
            if (!c->recv_pending && !c->poll_pending &&
                submit_recv(srv, c) != 0) {
                return -1;
            }
        }
        return 0;
    }
    close_conn(srv, c);
    return 0;
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
    if (!c->send_buf || sizeof(resp) - 1 >= c->send_cap) {
        return -1;
    }
    memcpy(c->send_buf, resp, sizeof(resp) - 1);
    if (srv->metrics) {
        srv->metrics->requests++;
        srv->metrics->responses_4xx++;
    }
    return begin_send_buf(srv, c, sizeof(resp) - 1, CS_SEND);
}

/** Start send of any bytes already queued in websocket out_buf. */
static int ws_flush_output(server_t *srv, conn_t *c)
{
    size_t flen;

    if (!c->ws || c->send_pending || c->state == CS_WS_SEND) {
        return 0;
    }
    flen = websocket_get_output(c->ws, (uint8_t *)c->send_buf, c->send_cap);
    if (flen == 0) {
        return 0;
    }
    if (flen >= c->send_cap) {
        return -1;
    }
    return begin_send_buf(srv, c, flen, CS_WS_SEND) == 0 ? 1 : -1;
}

/**
 * Drain one pending STATE_CHANGED text message into a WS frame on send_buf.
 * @return 1 started send, 0 nothing pending, -1 error.
 */
static int ws_try_send_pending(server_t *srv, conn_t *c)
{
    char text[EDGE_WS_MSG_MAX];
    size_t tlen = 0;
    size_t flen;
    int rc;

    if (!c->ws || !srv->hub) {
        return 0;
    }
    if (c->state == CS_WS_SEND || c->send_pending) {
        return 0;
    }
    /* Prefer draining control frames already queued (e.g. pong). */
    rc = ws_flush_output(srv, c);
    if (rc != 0) {
        return rc;
    }
    rc = edge_ws_hub_take_pending(srv->hub, c->slot, text, sizeof(text), &tlen);
    if (rc != 1) {
        return rc < 0 ? -1 : 0;
    }
    if (websocket_send_text(c->ws, text) != 0) {
        return -1;
    }
    flen = websocket_get_output(c->ws, (uint8_t *)c->send_buf, c->send_cap);
    if (flen == 0 || flen >= c->send_cap) {
        return -1;
    }
    return begin_send_buf(srv, c, flen, CS_WS_SEND) == 0 ? 1 : -1;
}

/** Fan-out: try to start sends on all WS subscribers with pending data. */
static void ws_flush_all_pending(server_t *srv)
{
    int i;
    for (i = 0; i < srv->max_conns; i++) {
        conn_t *c = &srv->conns[i];
        if (c->state == CS_WS_RECV && c->ws &&
            edge_ws_hub_is_subscribed(srv->hub, c->slot)) {
            (void)ws_try_send_pending(srv, c);
        }
    }
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
    if (srv->tls_ctx) {
        int want_w = 0;
        int hr;
        c->ssl = edge_tls_conn_new_fd(srv->tls_ctx, client_fd);
        if (!c->ssl) {
            close_conn(srv, c);
            return -1;
        }
        c->state = CS_TLS_HS;
        /* Drive handshake immediately; POLL only when OpenSSL wants I/O. */
        hr = edge_tls_handshake(c->ssl, &want_w);
        if (hr == 1) {
            c->state = CS_RECV;
            if (submit_recv(srv, c) != 0) {
                close_conn(srv, c);
                return -1;
            }
            return 0;
        }
        if (hr == 0) {
            if (submit_poll(srv, c, want_w) != 0) {
                close_conn(srv, c);
                return -1;
            }
            return 0;
        }
        close_conn(srv, c);
        return -1;
    }
    if (submit_recv(srv, c) != 0) {
        close_conn(srv, c);
        return -1;
    }
    return 0;
}

static int handle_http_bytes(server_t *srv, conn_t *c, size_t n)
{
    size_t rlen = 0;
    int pr;

    if (n == 0) {
        if (begin_bad_request_plain(srv, c) != 0) {
            return -1;
        }
        return 0;
    }
    pr = edge_http1_serve_feed(c->http, c->recv_buf, n, srv->metrics,
                               c->send_buf, c->send_cap, &rlen);
    if (pr < 0) {
        return -1;
    }
    if (pr == 0) {
        return submit_recv(srv, c);
    }
    c->keep_ws = edge_http1_serve_took_ws_upgrade(c->http);
    if (begin_send_buf(srv, c, rlen, CS_SEND) != 0) {
        return -1;
    }
    if (!c->keep_ws) {
        ws_flush_all_pending(srv);
    }
    return 0;
}

static int handle_tls_poll(server_t *srv, conn_t *c)
{
    int want_w = 0;
    int n;

    if (c->state == CS_TLS_HS) {
        n = edge_tls_handshake(c->ssl, &want_w);
        if (n == 1) {
            c->state = CS_RECV;
            return submit_recv(srv, c);
        }
        if (n == 0) {
            return submit_poll(srv, c, want_w);
        }
        return -1;
    }
    if (c->state == CS_RECV || c->state == CS_WS_RECV) {
        n = edge_tls_read(c->ssl, c->recv_buf, sizeof(c->recv_buf), &want_w);
        if (n > 0) {
            if (c->state == CS_WS_RECV) {
                if (handle_ws_recv(srv, c, (size_t)n) != 0) {
                    return -1;
                }
                if (c->state == CS_WS_RECV && !c->recv_pending &&
                    !c->poll_pending) {
                    return submit_recv(srv, c);
                }
                return 0;
            }
            return handle_http_bytes(srv, c, (size_t)n);
        }
        if (n == 0) {
            return submit_poll(srv, c, want_w);
        }
        if (n == -2) {
            /* EOF */
            if (c->state == CS_RECV) {
                return begin_bad_request_plain(srv, c);
            }
            return -1;
        }
        return -1;
    }
    if (c->state == CS_SEND || c->state == CS_WS_SEND) {
        return tls_drive_write(srv, c);
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

static int handle_ws_recv(server_t *srv, conn_t *c, size_t n)
{
    protocol_event_t ev;

    if (!c->ws) {
        return -1;
    }
    (void)websocket_feed_input(c->ws, c->recv_buf, n);
    if (srv->metrics) {
        srv->metrics->bytes_in += (uint64_t)n;
    }

    while (websocket_next_event(c->ws, &ev)) {
        if (ev.type == WS_EVENT_CLOSE || ev.type == WS_EVENT_ERROR ||
            ev.type == PROTOCOL_EVENT_ERROR) {
            return -1;
        }
        if (ev.type == WS_EVENT_PING) {
            const uint8_t *pdata = ev.u.ws_message.data;
            size_t plen = ev.u.ws_message.len;
            if (websocket_send_pong(c->ws, pdata, plen) != 0) {
                return -1;
            }
        }
        /* TEXT/BINARY/PONG from client ignored in v1 (server push only) */
    }
    /* Flush pong / any queued frames */
    if (c->state == CS_WS_RECV && !c->send_pending) {
        if (ws_flush_output(srv, c) < 0) {
            return -1;
        }
    }
    return 0;
}

static int promote_to_ws(server_t *srv, conn_t *c)
{
    c->ws = websocket_create(WS_ROLE_SERVER);
    if (!c->ws) {
        return -1;
    }
    if (edge_ws_hub_subscribe(srv->hub, c->slot) != 0) {
        websocket_destroy(c->ws);
        c->ws = NULL;
        return -1;
    }
    c->keep_ws = 0;
    c->state = CS_WS_RECV;
    /* destroy HTTP parser; no longer needed */
    if (c->http) {
        edge_http1_serve_destroy(c->http);
        c->http = NULL;
    }
    if (submit_recv(srv, c) != 0) {
        return -1;
    }
    (void)ws_try_send_pending(srv, c);
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
    {
        size_t maxf = cfg->static_max_file_bytes
                          ? cfg->static_max_file_bytes
                          : (64u * 1024u);
        srv.send_cap = maxf + 512u; /* headers + body */
        if (srv.send_cap < 4096u) {
            srv.send_cap = 4096u;
        }
        if (srv.send_cap < EDGE_WS_MSG_MAX + 32u) {
            srv.send_cap = EDGE_WS_MSG_MAX + 32u;
        }
    }
    srv.docroots.spa_root = cfg->spa_root[0] ? cfg->spa_root : NULL;
    srv.docroots.packages_root =
        cfg->packages_root[0] ? cfg->packages_root : NULL;
    srv.docroots.max_file_bytes = cfg->static_max_file_bytes;
    if (opts->state) {
        srv.store = opts->state;
        srv.store_owned = 0;
    } else {
        edge_state_config_t sc = edge_state_config_from_edge_config(cfg);
        srv.store = edge_state_create_with_config(&sc);
        srv.store_owned = 1;
    }
    /* Always apply ns flags from config (owned or external store; P1.14). */
    if (srv.store) {
        edge_state_apply_config(srv.store, cfg);
    }
    if (opts->metrics) {
        srv.metrics = opts->metrics;
        if (srv.metrics->started_at == 0) {
            edge_metrics_init(srv.metrics);
        }
    } else {
        edge_metrics_init(&srv.metrics_local);
        srv.metrics = &srv.metrics_local;
    }

    srv.auth = opts->auth;
    srv.plugins = opts->plugins;
    srv.service_api_key = opts->service_api_key;
    srv.e7 = opts->e7;
    srv.clickhouse = opts->clickhouse;
    srv.telemetry_user =
        cfg && cfg->clickhouse_telemetry_user[0]
            ? cfg->clickhouse_telemetry_user
            : NULL;
    srv.telemetry_password =
        cfg ? cfg->clickhouse_telemetry_password : NULL;
    srv.ca = opts->ca;
    if (srv.e7 && srv.clickhouse) {
        edge_e7_callhome_set_clickhouse(srv.e7, srv.clickhouse);
    }
    srv.e7_accept_pending = 0;
    srv.mono_ms = mono_now_ms();
    if (edge_tls_config_enabled(cfg->tls_cert, cfg->tls_key)) {
        edge_tls_config_t tc;
        memset(&tc, 0, sizeof(tc));
        tc.cert_file = cfg->tls_cert;
        tc.key_file = cfg->tls_key;
        tc.client_ca_file =
            cfg->tls_client_ca[0] ? cfg->tls_client_ca : NULL;
        srv.tls_ctx = edge_tls_ctx_create(&tc);
        if (!srv.tls_ctx) {
            if (srv.store_owned && srv.store) {
                edge_state_destroy(srv.store);
            }
            return 1;
        }
    }
    edge_pq_sidecar_config_defaults(&srv.pq_cfg);
    srv.pq_cfg.enabled = cfg->pqproxy_enabled;
    if (cfg->pqproxy_metrics_url[0]) {
        snprintf(srv.pq_cfg.metrics_url, sizeof(srv.pq_cfg.metrics_url), "%s",
                 cfg->pqproxy_metrics_url);
    }
    srv.pq_cfg.scrape_interval_ms =
        cfg->pqproxy_scrape_interval_ms ? cfg->pqproxy_scrape_interval_ms
                                        : 5000;
    srv.last_scrape_ms = 0;
    srv.hub = edge_ws_hub_create((size_t)srv.max_conns);
    if (!srv.hub) {
        edge_tls_ctx_free(srv.tls_ctx);
        if (srv.store_owned && srv.store) {
            edge_state_destroy(srv.store);
        }
        return 1;
    }
    /* Plugin host API state_put → STATE_CHANGED (PR-1 / K6). */
    if (srv.plugins) {
        edge_plugin_host_set_ws_hub(srv.plugins, srv.hub);
    }
    /* E7 Call Home coalesce notify (PR-4b / K16). */
    if (srv.e7) {
        edge_e7_callhome_set_hub(srv.e7, srv.hub);
    }

    srv.conns = (conn_t *)host_alloc_kind(
        EDGE_MEM_HTTP, (size_t)srv.max_conns * sizeof(conn_t));
    if (!srv.conns) {
        edge_ws_hub_destroy(srv.hub);
        edge_tls_ctx_free(srv.tls_ctx);
        if (srv.store_owned && srv.store) {
            edge_state_destroy(srv.store);
        }
        return 1;
    }
    for (i = 0; i < srv.max_conns; i++) {
        srv.conns[i].fd = -1;
        srv.conns[i].state = CS_FREE;
        srv.conns[i].slot = i;
    }

    srv.listen_fd = create_listen_socket(cfg, opts->backlog);
    if (srv.listen_fd < 0) {
        host_free(srv.conns);
        edge_ws_hub_destroy(srv.hub);
        edge_tls_ctx_free(srv.tls_ctx);
        if (srv.store_owned && srv.store) {
            edge_state_destroy(srv.store);
        }
        return 1;
    }

    {
        int entries = opts->ring_entries > 0 ? opts->ring_entries : 256;
        rc = io_uring_queue_init(entries, &srv.ring, 0);
        if (rc < 0) {
            fprintf(stderr, "edgehost: io_uring_queue_init: %s\n",
                    strerror(-rc));
            close(srv.listen_fd);
            host_free(srv.conns);
            edge_ws_hub_destroy(srv.hub);
            edge_tls_ctx_free(srv.tls_ctx);
            if (srv.store_owned && srv.store) {
                edge_state_destroy(srv.store);
            }
            return 1;
        }
    }

    /* Optional E7 Call Home second listen (bind if not already). */
    if (srv.e7 && edge_e7_callhome_enabled(srv.e7)) {
        if (edge_e7_callhome_listen_fd(srv.e7) < 0) {
            if (edge_e7_callhome_bind(srv.e7) != 0) {
                fprintf(stderr, "edgehost: e7_callhome bind failed\n");
                io_uring_queue_exit(&srv.ring);
                close(srv.listen_fd);
                host_free(srv.conns);
                edge_ws_hub_destroy(srv.hub);
                edge_tls_ctx_free(srv.tls_ctx);
                if (srv.store_owned && srv.store) {
                    edge_state_destroy(srv.store);
                }
                return 1;
            }
        }
    }

    if (submit_accept(&srv) != 0) {
        io_uring_queue_exit(&srv.ring);
        close(srv.listen_fd);
        host_free(srv.conns);
        edge_ws_hub_destroy(srv.hub);
        edge_tls_ctx_free(srv.tls_ctx);
        if (srv.store_owned && srv.store) {
            edge_state_destroy(srv.store);
        }
        return 1;
    }
    if (srv.e7 && edge_e7_callhome_listen_fd(srv.e7) >= 0) {
        if (submit_e7_accept(&srv) != 0) {
            fprintf(stderr, "edgehost: e7 accept submit failed\n");
            io_uring_queue_exit(&srv.ring);
            close(srv.listen_fd);
            host_free(srv.conns);
            edge_ws_hub_destroy(srv.hub);
            edge_tls_ctx_free(srv.tls_ctx);
            if (srv.store_owned && srv.store) {
                edge_state_destroy(srv.store);
            }
            return 1;
        }
    }
    io_uring_submit(&srv.ring);

    fprintf(stderr,
            "edgehost: listening on %s:%u tls=%s (SPA=%s packages=%s)\n",
            cfg->listen_host[0] ? cfg->listen_host : "0.0.0.0",
            (unsigned)cfg->listen_port,
            srv.tls_ctx ? "on" : "plain",
            cfg->spa_root[0] ? cfg->spa_root : "(none)",
            cfg->packages_root[0] ? cfg->packages_root : "(none)");

    while (!should_stop(&srv)) {
        struct io_uring_cqe *cqe = NULL;
        struct __kernel_timespec ts;
        uint8_t domain;
        uint32_t uop, uslot;
        int op, slot;
        int res;

        ts.tv_sec = 0;
        /*
         * Default 200 ms wait is fine for idle Call Home. During get-config
         * capture, drop to 2 ms so we re-read as soon as WINDOW_ADJUST lets
         * the peer send the next SSH packets (avoids multi-minute crawls).
         */
        if (edge_debug_cpu_profiling()) {
            ts.tv_nsec = 10L * 1000L * 1000L;
        } else if (srv.e7 && edge_e7_callhome_has_inflight_cmds(srv.e7)) {
            ts.tv_nsec = 2L * 1000L * 1000L;
        } else {
            ts.tv_nsec = 200L * 1000L * 1000L;
        }
        rc = io_uring_wait_cqe_timeout(&srv.ring, &cqe, &ts);
        srv.mono_ms = mono_now_ms();
        edge_debug_cpu_on_tick(srv.mono_ms);
        /* ADR-005 / K15: SIGHUP → shadow YAML → apply edgecore + state + e7 */
        try_hup_reload(&srv);
        if (rc == -ETIME || rc == -EAGAIN) {
            if (should_stop(&srv)) {
                break;
            }
            publish_http_debug(&srv);
            /* opportunistic fan-out flush */
            ws_flush_all_pending(&srv);
            /* PR-4a: E7 session pump (identity + CLIENT hello I/O) */
            if (srv.e7) {
                edge_e7_callhome_on_tick(srv.e7, srv.mono_ms);
            }
            if (srv.clickhouse) {
                edge_clickhouse_on_tick(srv.clickhouse, srv.mono_ms);
            }
            /* P1.11: periodic pqproxy metrics scrape */
            if (srv.pq_cfg.enabled && srv.store) {
                static uint64_t tick;
                /* approx wait: 2 ms when capturing, else 200 ms */
                tick += (srv.e7 && edge_e7_callhome_has_inflight_cmds(srv.e7))
                            ? 2u
                            : 200u;
                if (tick - srv.last_scrape_ms >=
                    (uint64_t)srv.pq_cfg.scrape_interval_ms) {
                    srv.last_scrape_ms = tick;
                    (void)edge_pq_sidecar_scrape_once(&srv.pq_cfg, srv.store,
                                                      srv.hub);
                    ws_flush_all_pending(&srv);
                }
            }
            (void)submit_e7_accept(&srv);
            io_uring_submit(&srv.ring);
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
        unpack_ud3(cqe->user_data, &domain, &uop, &uslot);
        op = (int)uop;
        slot = (int)uslot;
        io_uring_cqe_seen(&srv.ring, cqe);

        if (domain == UD_DOMAIN_E7) {
            if (op == E7_OP_ACCEPT) {
                srv.e7_accept_pending = 0;
                if (res >= 0 && srv.e7) {
                    (void)edge_e7_callhome_on_accept(
                        srv.e7, res,
                        (struct sockaddr *)&srv.e7_client_addr,
                        srv.e7_client_addr_len);
                    edge_e7_callhome_on_tick(srv.e7, srv.mono_ms);
                } else if (res < 0 && res != -EAGAIN && res != -ECONNABORTED &&
                           res != -EINTR) {
                    fprintf(stderr, "edgehost: e7 accept: %s\n",
                            strerror(-res));
                }
                (void)submit_e7_accept(&srv);
            }
            /* TODO PR-4+: full E7 RECV/SEND CQE demux; tick poll for now */
            io_uring_submit(&srv.ring);
            continue;
        }

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
            if (!c || c->state == CS_FREE) {
                /* late cqe */
            } else if (c->state == CS_WS_RECV) {
                c->recv_pending = 0;
                if (res <= 0) {
                    close_conn(&srv, c);
                } else if (handle_ws_recv(&srv, c, (size_t)res) != 0) {
                    close_conn(&srv, c);
                } else if (c->state == CS_WS_RECV && !c->recv_pending) {
                    if (submit_recv(&srv, c) != 0) {
                        close_conn(&srv, c);
                    }
                }
            } else if (c->state == CS_RECV) {
                c->recv_pending = 0;
                if (res < 0) {
                    close_conn(&srv, c);
                } else if (handle_http_bytes(&srv, c, (size_t)res) != 0) {
                    close_conn(&srv, c);
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
                        if (handle_send_complete(&srv, c) != 0) {
                            close_conn(&srv, c);
                        }
                    } else if (submit_send(&srv, c) != 0) {
                        close_conn(&srv, c);
                    }
                }
            }
        } else if (op == OP_POLL) {
            conn_t *c = (slot >= 0 && slot < srv.max_conns)
                            ? &srv.conns[slot]
                            : NULL;
            if (c && c->state != CS_FREE) {
                c->poll_pending = 0;
                if (res < 0) {
                    close_conn(&srv, c);
                } else if (c->ssl) {
                    if (handle_tls_poll(&srv, c) != 0) {
                        close_conn(&srv, c);
                    }
                }
            }
        }

        if (srv.e7) {
            edge_e7_callhome_on_tick(srv.e7, srv.mono_ms);
            (void)submit_e7_accept(&srv);
        }
        io_uring_submit(&srv.ring);
    }

    for (i = 0; i < srv.max_conns; i++) {
        close_conn(&srv, &srv.conns[i]);
    }
    if (srv.e7) {
        /* Leave sessions; caller owns destroy. Close listen only if we bound. */
        edge_e7_callhome_close_all(srv.e7);
    }
    io_uring_queue_exit(&srv.ring);
    close(srv.listen_fd);
    host_free(srv.conns);
    edge_ws_hub_destroy(srv.hub);
    edge_tls_ctx_free(srv.tls_ctx);
    if (srv.store_owned && srv.store) {
        edge_state_destroy(srv.store);
    }
    fprintf(stderr, "edgehost: iouring loop exit (%d accepts)\n",
            srv.accepts_done);
    return exit_code;
}
