/**
 * @file edge_pg.c
 * @brief Non-blocking Postgres simple-query over Unix domain socket.
 */

#define _GNU_SOURCE

#include "edge_pg.h"

#include "host_alloc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct edge_pg_conn {
    int              fd;
    edge_pg_config_t cfg;
    char             err[EDGE_PG_ERR_MAX];
};

void edge_pg_config_defaults(edge_pg_config_t *c)
{
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
    snprintf(c->sock_path, sizeof(c->sock_path),
             "/var/run/postgresql/.s.PGSQL.5432");
    snprintf(c->database, sizeof(c->database), "edgehost");
    snprintf(c->user, sizeof(c->user), "edgehost");
    c->timeout_ms = 5000;
}

static int set_nb(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static int poll_fd(int fd, int want_write, uint32_t timeout_ms)
{
    struct pollfd p;
    p.fd = fd;
    p.events = want_write ? POLLOUT : POLLIN;
    p.revents = 0;
    return poll(&p, 1, (int)timeout_ms);
}

static int write_all(int fd, const void *buf, size_t len, uint32_t timeout_ms)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    uint64_t deadline = now_ms() + timeout_ms;

    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                      errno == EINTR)) {
            uint64_t left = deadline > now_ms() ? deadline - now_ms() : 0;
            if (left == 0 || poll_fd(fd, 1, (uint32_t)left) <= 0) {
                return -1;
            }
            continue;
        }
        return -1;
    }
    return 0;
}

static int read_some(int fd, void *buf, size_t cap, size_t *got,
                     uint32_t timeout_ms)
{
    uint64_t deadline = now_ms() + timeout_ms;
    *got = 0;
    for (;;) {
        ssize_t n = read(fd, buf, cap);
        if (n > 0) {
            *got = (size_t)n;
            return 0;
        }
        if (n == 0) {
            return -1; /* EOF */
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            uint64_t left = deadline > now_ms() ? deadline - now_ms() : 0;
            if (left == 0 || poll_fd(fd, 0, (uint32_t)left) <= 0) {
                return -1;
            }
            continue;
        }
        return -1;
    }
}

static void put_i32(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xff);
    p[1] = (uint8_t)((v >> 16) & 0xff);
    p[2] = (uint8_t)((v >> 8) & 0xff);
    p[3] = (uint8_t)(v & 0xff);
}

static int32_t get_i32(const uint8_t *p)
{
    return ((int32_t)p[0] << 24) | ((int32_t)p[1] << 16) |
           ((int32_t)p[2] << 8) | (int32_t)p[3];
}

static int16_t get_i16(const uint8_t *p)
{
    return (int16_t)(((int16_t)p[0] << 8) | (int16_t)p[1]);
}

static int startup(edge_pg_conn_t *c)
{
    /* StartupMessage: int32 len | int32 196608 | user\0 | val\0 | database\0 | val\0 | \0 */
    uint8_t msg[512];
    size_t o = 4;
    put_i32(msg + o, 196608); /* protocol 3.0 */
    o += 4;
    o += (size_t)snprintf((char *)msg + o, sizeof(msg) - o, "user");
    msg[o++] = 0;
    o += (size_t)snprintf((char *)msg + o, sizeof(msg) - o, "%s", c->cfg.user);
    msg[o++] = 0;
    o += (size_t)snprintf((char *)msg + o, sizeof(msg) - o, "database");
    msg[o++] = 0;
    o += (size_t)snprintf((char *)msg + o, sizeof(msg) - o, "%s",
                          c->cfg.database);
    msg[o++] = 0;
    msg[o++] = 0;
    put_i32(msg, (int32_t)o);
    return write_all(c->fd, msg, o, c->cfg.timeout_ms);
}

static int send_password(edge_pg_conn_t *c, const char *pass)
{
    uint8_t msg[256];
    size_t plen = strlen(pass);
    size_t o = 0;
    msg[o++] = 'p';
    put_i32(msg + o, (int32_t)(4 + plen + 1));
    o += 4;
    memcpy(msg + o, pass, plen);
    o += plen;
    msg[o++] = 0;
    return write_all(c->fd, msg, o, c->cfg.timeout_ms);
}

static int read_message(edge_pg_conn_t *c, char *type, uint8_t *body,
                        size_t body_cap, size_t *body_len)
{
    uint8_t hdr[5];
    size_t got = 0;
    int32_t len;
    size_t need;
    size_t off = 0;

    if (read_some(c->fd, hdr, 5, &got, c->cfg.timeout_ms) != 0 || got < 5) {
        return -1;
    }
    *type = (char)hdr[0];
    len = get_i32(hdr + 1);
    if (len < 4) {
        return -1;
    }
    need = (size_t)len - 4;
    if (need > body_cap) {
        /* drain oversized */
        uint8_t dump[1024];
        size_t left = need;
        while (left) {
            size_t chunk = left > sizeof(dump) ? sizeof(dump) : left;
            size_t g = 0;
            if (read_some(c->fd, dump, chunk, &g, c->cfg.timeout_ms) != 0) {
                return -1;
            }
            left -= g;
        }
        *body_len = 0;
        return -1;
    }
    while (off < need) {
        size_t g = 0;
        if (read_some(c->fd, body + off, need - off, &g, c->cfg.timeout_ms) !=
            0) {
            return -1;
        }
        off += g;
    }
    *body_len = need;
    return 0;
}

static int auth_and_ready(edge_pg_conn_t *c)
{
    for (;;) {
        char type = 0;
        uint8_t body[4096];
        size_t blen = 0;
        if (read_message(c, &type, body, sizeof(body), &blen) != 0) {
            snprintf(c->err, sizeof(c->err), "read during auth failed");
            return -1;
        }
        if (type == 'R') {
            int32_t code = blen >= 4 ? get_i32(body) : -1;
            if (code == 0) {
                continue; /* AuthenticationOk */
            }
            if (code == 3) {
                /* cleartext password */
                if (send_password(c, c->cfg.password[0] ? c->cfg.password
                                                        : "") != 0) {
                    return -1;
                }
                continue;
            }
            if (code == 5) {
                snprintf(c->err, sizeof(c->err),
                         "MD5 auth not supported; use trust/peer or cleartext");
                return -1;
            }
            snprintf(c->err, sizeof(c->err), "unsupported auth type %d", code);
            return -1;
        }
        if (type == 'E') {
            /* ErrorResponse — copy message field 'M' */
            size_t i = 0;
            while (i + 1 < blen) {
                char f = (char)body[i++];
                if (f == 0) {
                    break;
                }
                if (f == 'M') {
                    snprintf(c->err, sizeof(c->err), "%s", (char *)body + i);
                    return -1;
                }
                while (i < blen && body[i] != 0) {
                    i++;
                }
                if (i < blen) {
                    i++;
                }
            }
            snprintf(c->err, sizeof(c->err), "auth error");
            return -1;
        }
        if (type == 'Z') {
            return 0; /* ReadyForQuery */
        }
        if (type == 'K' || type == 'S' || type == 'N') {
            continue; /* BackendKeyData, ParameterStatus, Notice */
        }
    }
}

edge_pg_conn_t *edge_pg_connect(const edge_pg_config_t *cfg)
{
    edge_pg_conn_t *c;
    struct sockaddr_un addr;
    int rc;

    if (!cfg || !cfg->sock_path[0]) {
        return NULL;
    }
    c = (edge_pg_conn_t *)host_alloc_kind(EDGE_MEM_OTHER, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->cfg = *cfg;
    if (c->cfg.timeout_ms == 0) {
        c->cfg.timeout_ms = 5000;
    }
    c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c->fd < 0) {
        host_free(c);
        return NULL;
    }
    if (set_nb(c->fd) != 0) {
        close(c->fd);
        host_free(c);
        return NULL;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(cfg->sock_path) >= sizeof(addr.sun_path)) {
        close(c->fd);
        host_free(c);
        return NULL;
    }
    memcpy(addr.sun_path, cfg->sock_path, strlen(cfg->sock_path) + 1);
    rc = connect(c->fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS && errno != EAGAIN) {
        close(c->fd);
        host_free(c);
        return NULL;
    }
    if (rc < 0) {
        if (poll_fd(c->fd, 1, c->cfg.timeout_ms) <= 0) {
            close(c->fd);
            host_free(c);
            return NULL;
        }
        {
            int err = 0;
            socklen_t el = sizeof(err);
            if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 ||
                err != 0) {
                close(c->fd);
                host_free(c);
                return NULL;
            }
        }
    }
    if (startup(c) != 0 || auth_and_ready(c) != 0) {
        close(c->fd);
        host_free(c);
        return NULL;
    }
    return c;
}

void edge_pg_close(edge_pg_conn_t *c)
{
    if (!c) {
        return;
    }
    if (c->fd >= 0) {
        uint8_t term[5] = {'X', 0, 0, 0, 4};
        (void)write_all(c->fd, term, 5, 200);
        close(c->fd);
    }
    host_free(c);
}

void edge_pg_result_clear(edge_pg_result_t *r)
{
    int i, j;
    if (!r) {
        return;
    }
    for (i = 0; i < r->n_rows; i++) {
        for (j = 0; j < r->n_cols; j++) {
            host_free(r->cells[i][j]);
            r->cells[i][j] = NULL;
        }
    }
    r->n_rows = 0;
    r->n_cols = 0;
    r->ok = 0;
    r->err[0] = '\0';
}

const char *edge_pg_cell(const edge_pg_result_t *r, int row, int col)
{
    if (!r || row < 0 || col < 0 || row >= r->n_rows || col >= r->n_cols) {
        return NULL;
    }
    return r->cells[row][col];
}

int edge_pg_escape_literal(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    size_t i;
    if (!out || out_sz < 3) {
        return -1;
    }
    out[o++] = '\'';
    if (in) {
        for (i = 0; in[i]; i++) {
            if (in[i] == '\'') {
                if (o + 2 >= out_sz) {
                    return -1;
                }
                out[o++] = '\'';
                out[o++] = '\'';
            } else {
                if (o + 1 >= out_sz) {
                    return -1;
                }
                out[o++] = in[i];
            }
        }
    }
    if (o + 1 >= out_sz) {
        return -1;
    }
    out[o++] = '\'';
    out[o] = '\0';
    return 0;
}

int edge_pg_dollar_tag(const char *body, size_t body_len, char *tag,
                       size_t tag_sz)
{
    int n;
    char cand[32];
    if (!tag || tag_sz < 8) {
        return -1;
    }
    for (n = 0; n < 64; n++) {
        int k = snprintf(cand, sizeof(cand), "$ehcfg%d$", n);
        size_t i;
        int hit = 0;
        if (k < 0) {
            return -1;
        }
        if (!body || body_len == 0) {
            snprintf(tag, tag_sz, "%s", cand);
            return 0;
        }
        for (i = 0; i + (size_t)k <= body_len; i++) {
            if (memcmp(body + i, cand, (size_t)k) == 0) {
                hit = 1;
                break;
            }
        }
        if (!hit) {
            snprintf(tag, tag_sz, "%s", cand);
            return 0;
        }
    }
    return -1;
}

static int edge_pg_exec_wire(edge_pg_conn_t *c, const char *sql, size_t slen,
                             edge_pg_result_t *out)
{
    uint8_t *qbuf = NULL;
    size_t qcap;
    size_t o = 0;
    char type;
    uint8_t body[65536];
    size_t blen;

    if (!c || !sql || !out) {
        return -1;
    }
    edge_pg_result_clear(out);
    qcap = 1 + 4 + slen + 1;
    qbuf = (uint8_t *)host_alloc_kind(EDGE_MEM_OTHER, qcap);
    if (!qbuf) {
        snprintf(out->err, sizeof(out->err), "OOM query buffer");
        return -1;
    }
    qbuf[o++] = 'Q';
    put_i32(qbuf + o, (int32_t)(4 + slen + 1));
    o += 4;
    memcpy(qbuf + o, sql, slen);
    o += slen;
    qbuf[o++] = 0;
    if (write_all(c->fd, qbuf, o, c->cfg.timeout_ms) != 0) {
        snprintf(out->err, sizeof(out->err), "write query failed");
        host_free(qbuf);
        return -1;
    }
    host_free(qbuf);
    qbuf = NULL;

    for (;;) {
        if (read_message(c, &type, body, sizeof(body), &blen) != 0) {
            snprintf(out->err, sizeof(out->err), "read response failed");
            return -1;
        }
        if (type == 'T') {
            /* RowDescription */
            int16_t ncols;
            size_t p = 0;
            int i;
            if (blen < 2) {
                return -1;
            }
            ncols = get_i16(body);
            if (ncols > EDGE_PG_COLS_MAX) {
                ncols = EDGE_PG_COLS_MAX;
            }
            out->n_cols = ncols;
            p = 2;
            for (i = 0; i < ncols; i++) {
                size_t nlen = 0;
                while (p + nlen < blen && body[p + nlen] != 0) {
                    nlen++;
                }
                if (nlen >= sizeof(out->colnames[i])) {
                    nlen = sizeof(out->colnames[i]) - 1;
                }
                memcpy(out->colnames[i], body + p, nlen);
                out->colnames[i][nlen] = '\0';
                p += nlen + 1;
                p += 18; /* tableoid, col, type, typlen, typmod, format */
                if (p > blen) {
                    break;
                }
            }
            continue;
        }
        if (type == 'D') {
            /* DataRow */
            int16_t ncols;
            size_t p = 0;
            int i;
            int row;
            if (out->n_rows >= EDGE_PG_ROWS_MAX) {
                continue;
            }
            if (blen < 2) {
                return -1;
            }
            ncols = get_i16(body);
            p = 2;
            row = out->n_rows;
            for (i = 0; i < ncols && i < EDGE_PG_COLS_MAX; i++) {
                int32_t clen;
                char *cell;
                if (p + 4 > blen) {
                    break;
                }
                clen = get_i32(body + p);
                p += 4;
                if (clen < 0) {
                    out->cells[row][i] = NULL;
                    continue;
                }
                if (p + (size_t)clen > blen) {
                    break;
                }
                cell = (char *)host_alloc_kind(EDGE_MEM_OTHER, (size_t)clen + 1);
                if (!cell) {
                    return -1;
                }
                memcpy(cell, body + p, (size_t)clen);
                cell[clen] = '\0';
                out->cells[row][i] = cell;
                p += (size_t)clen;
            }
            out->n_rows++;
            continue;
        }
        if (type == 'C') {
            out->ok = 1;
            continue;
        }
        if (type == 'E') {
            size_t i = 0;
            out->ok = 0;
            while (i + 1 < blen) {
                char f = (char)body[i++];
                if (f == 0) {
                    break;
                }
                if (f == 'M') {
                    snprintf(out->err, sizeof(out->err), "%s",
                             (char *)body + i);
                    break;
                }
                while (i < blen && body[i] != 0) {
                    i++;
                }
                if (i < blen) {
                    i++;
                }
            }
            continue;
        }
        if (type == 'Z') {
            return 0;
        }
        if (type == 'I' || type == 'n' || type == 'N' || type == 'A') {
            continue;
        }
    }
}

int edge_pg_exec(edge_pg_conn_t *c, const char *sql, edge_pg_result_t *out)
{
    if (!sql) {
        return -1;
    }
    return edge_pg_exec_wire(c, sql, strlen(sql), out);
}

int edge_pg_exec_large(edge_pg_conn_t *c, const char *sql, size_t sql_len,
                       edge_pg_result_t *out)
{
    if (!sql) {
        return -1;
    }
    return edge_pg_exec_wire(c, sql, sql_len, out);
}
