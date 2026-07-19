/**
 * @file outbound_http.c
 * @brief Blocking outbound HTTP/1.1 (+ OpenSSL HTTPS) for PENDING (P1.8b).
 */

#define _GNU_SOURCE

#include "edge_outbound.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

void edge_outbound_opts_defaults(edge_outbound_opts_t *o)
{
    if (!o) {
        return;
    }
    memset(o, 0, sizeof(*o));
    o->allow_blocking_dns = 0;
    o->max_response_body = 4u * 1024u * 1024u;
    o->default_timeout_ms = 60000;
}

typedef struct {
    int  https;
    char host[256];
    char path[1024];
    uint16_t port;
} parsed_url_t;

static int parse_url(const char *url, parsed_url_t *u)
{
    const char *p;
    const char *slash;
    size_t hlen;

    if (!url || !u) {
        return -1;
    }
    memset(u, 0, sizeof(*u));
    if (strncmp(url, "https://", 8) == 0) {
        u->https = 1;
        u->port = 443;
        p = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        u->https = 0;
        u->port = 80;
        p = url + 7;
    } else {
        return -1;
    }
    slash = strchr(p, '/');
    if (slash) {
        hlen = (size_t)(slash - p);
        snprintf(u->path, sizeof(u->path), "%s", slash);
    } else {
        hlen = strlen(p);
        snprintf(u->path, sizeof(u->path), "/");
    }
    if (hlen == 0 || hlen >= sizeof(u->host)) {
        return -1;
    }
    memcpy(u->host, p, hlen);
    u->host[hlen] = '\0';
    /* strip userinfo if any */
    {
        char *at = strchr(u->host, '@');
        if (at) {
            memmove(u->host, at + 1, strlen(at + 1) + 1);
        }
    }
    {
        char *c = strrchr(u->host, ':');
        if (c && strchr(u->host, ']') == NULL) {
            long port = strtol(c + 1, NULL, 10);
            if (port > 0 && port <= 65535) {
                u->port = (uint16_t)port;
                *c = '\0';
            }
        }
    }
    return u->host[0] ? 0 : -1;
}

static int tcp_connect_host(const char *host, uint16_t port,
                            const char *addr_override, int allow_dns, int *out_fd)
{
    int fd = -1;
    struct sockaddr_in addr;
    char portstr[16];

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (addr_override && addr_override[0]) {
        if (inet_pton(AF_INET, addr_override, &addr.sin_addr) != 1) {
            return -1;
        }
        fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return -1;
        }
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            close(fd);
            return -1;
        }
        *out_fd = fd;
        return 0;
    }

    if (!allow_dns) {
        return -1;
    }

    {
        struct addrinfo hints;
        struct addrinfo *res = NULL;
        struct addrinfo *rp;
        int rc;

        memset(&hints, 0, sizeof(hints));
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_INET;
        snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
        rc = getaddrinfo(host, portstr, &hints, &res);
        if (rc != 0) {
            return -1;
        }
        for (rp = res; rp; rp = rp->ai_next) {
            fd = socket(rp->ai_family, rp->ai_socktype | SOCK_CLOEXEC,
                        rp->ai_protocol);
            if (fd < 0) {
                continue;
            }
            if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
                break;
            }
            close(fd);
            fd = -1;
        }
        freeaddrinfo(res);
        if (fd < 0) {
            return -1;
        }
        *out_fd = fd;
        return 0;
    }
}

static int build_request(const edge_http_client_req_t *req, const parsed_url_t *u,
                         const char *host_hdr, char *out, size_t out_cap,
                         size_t *out_len)
{
    size_t off = 0;
    int n;
    size_t i;

    n = snprintf(out + off, out_cap - off, "%s %s HTTP/1.1\r\nHost: %s\r\n",
                 req->method ? req->method : "GET", u->path,
                 host_hdr && host_hdr[0] ? host_hdr : u->host);
    if (n < 0 || (size_t)n >= out_cap - off) {
        return -1;
    }
    off += (size_t)n;
    n = snprintf(out + off, out_cap - off, "Connection: close\r\n");
    if (n < 0 || (size_t)n >= out_cap - off) {
        return -1;
    }
    off += (size_t)n;

    for (i = 0; i < req->n_headers; i++) {
        if (!req->hdr_names || !req->hdr_values || !req->hdr_names[i]) {
            continue;
        }
        n = snprintf(out + off, out_cap - off, "%s: %s\r\n", req->hdr_names[i],
                     req->hdr_values[i] ? req->hdr_values[i] : "");
        if (n < 0 || (size_t)n >= out_cap - off) {
            return -1;
        }
        off += (size_t)n;
    }
    if (req->body && req->body_len > 0) {
        n = snprintf(out + off, out_cap - off, "Content-Length: %zu\r\n\r\n",
                     req->body_len);
    } else {
        n = snprintf(out + off, out_cap - off, "Content-Length: 0\r\n\r\n");
    }
    if (n < 0 || (size_t)n >= out_cap - off) {
        return -1;
    }
    off += (size_t)n;
    if (req->body && req->body_len > 0) {
        if (off + req->body_len >= out_cap) {
            return -1;
        }
        memcpy(out + off, req->body, req->body_len);
        off += req->body_len;
    }
    *out_len = off;
    return 0;
}

static int parse_status_and_body(const uint8_t *raw, size_t raw_len, int *status,
                                 const uint8_t **body, size_t *body_len)
{
    const char *p;
    const char *end;
    long st;
    const uint8_t *hdr_end = NULL;
    size_t i;

    if (!raw || raw_len < 12 || !status || !body || !body_len) {
        return -1;
    }
    p = (const char *)raw;
    if (strncmp(p, "HTTP/1.", 7) != 0) {
        return -1;
    }
    p = strchr(p, ' ');
    if (!p) {
        return -1;
    }
    st = strtol(p + 1, (char **)&end, 10);
    if (end == p + 1 || st < 100 || st > 599) {
        return -1;
    }
    *status = (int)st;

    for (i = 0; i + 3 < raw_len; i++) {
        if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' &&
            raw[i + 3] == '\n') {
            hdr_end = raw + i + 4;
            break;
        }
    }
    if (!hdr_end) {
        return -1;
    }
    *body = hdr_end;
    *body_len = (size_t)(raw + raw_len - hdr_end);
    return 0;
}

static int io_write_all_fd(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int io_read_all_fd(int fd, uint8_t *buf, size_t cap, size_t *out_len)
{
    size_t off = 0;
    while (off < cap) {
        ssize_t n = read(fd, buf + off, cap - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        off += (size_t)n;
    }
    *out_len = off;
    return 0;
}

static int io_write_all_ssl(SSL *ssl, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        int n = SSL_write(ssl, p, (int)len);
        if (n <= 0) {
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int io_read_all_ssl(SSL *ssl, uint8_t *buf, size_t cap, size_t *out_len)
{
    size_t off = 0;
    while (off < cap) {
        int n = SSL_read(ssl, buf + off, (int)(cap - off));
        if (n <= 0) {
            break;
        }
        off += (size_t)n;
    }
    *out_len = off;
    return 0;
}

int edge_outbound_http_execute(const edge_http_client_req_t *req,
                               const edge_outbound_opts_t *opts,
                               edge_http_client_result_t *out, uint8_t *body_buf,
                               size_t body_cap, size_t *body_len)
{
    edge_outbound_opts_t local;
    parsed_url_t u;
    int fd = -1;
    char *reqbuf = NULL;
    size_t req_len = 0;
    uint8_t *raw = NULL;
    size_t raw_cap;
    size_t raw_len = 0;
    int status = 0;
    const uint8_t *bptr = NULL;
    size_t blen = 0;
    const char *host_hdr;
    int allow_dns;
    size_t max_body;
    SSL_CTX *ssl_ctx = NULL;
    SSL *ssl = NULL;
    int rc = -1;

    if (!req || !out || !body_buf || !body_len) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    *body_len = 0;

    if (!opts) {
        edge_outbound_opts_defaults(&local);
        opts = &local;
    }
    allow_dns = opts->allow_blocking_dns;
    max_body = opts->max_response_body ? opts->max_response_body
                                       : (4u * 1024u * 1024u);
    if (body_cap < 64) {
        out->transport_err = ENOMEM;
        return 0;
    }

    if (parse_url(req->url, &u) != 0) {
        out->transport_err = EINVAL;
        return 0;
    }
    host_hdr = req->host && req->host[0] ? req->host : u.host;

    {
        size_t hdr_est = 4096 + req->body_len + 256;
        reqbuf = (char *)malloc(hdr_est);
        if (!reqbuf) {
            out->transport_err = ENOMEM;
            return 0;
        }
        if (build_request(req, &u, host_hdr, reqbuf, hdr_est, &req_len) != 0) {
            free(reqbuf);
            out->transport_err = EMSGSIZE;
            return 0;
        }
    }

    if (tcp_connect_host(u.host, u.port, req->addr_override, allow_dns, &fd) !=
        0) {
        free(reqbuf);
        out->transport_err = ECONNREFUSED;
        return 0;
    }

    raw_cap = max_body + 8192;
    if (raw_cap < 16384) {
        raw_cap = 16384;
    }
    raw = (uint8_t *)malloc(raw_cap);
    if (!raw) {
        close(fd);
        free(reqbuf);
        out->transport_err = ENOMEM;
        return 0;
    }

    if (u.https) {
        const SSL_METHOD *method;
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
        method = TLS_client_method();
#else
        method = SSLv23_client_method();
#endif
        ssl_ctx = SSL_CTX_new(method);
        if (!ssl_ctx) {
            out->transport_err = EIO;
            goto done;
        }
        SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL); /* lab; pin later */
        ssl = SSL_new(ssl_ctx);
        if (!ssl || SSL_set_fd(ssl, fd) != 1) {
            out->transport_err = EIO;
            goto done;
        }
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
        (void)SSL_set_tlsext_host_name(ssl, host_hdr);
#endif
        if (SSL_connect(ssl) != 1) {
            out->transport_err = ECONNREFUSED;
            goto done;
        }
        if (io_write_all_ssl(ssl, reqbuf, req_len) != 0 ||
            io_read_all_ssl(ssl, raw, raw_cap, &raw_len) != 0) {
            out->transport_err = EIO;
            goto done;
        }
    } else {
        if (io_write_all_fd(fd, reqbuf, req_len) != 0 ||
            io_read_all_fd(fd, raw, raw_cap, &raw_len) != 0) {
            out->transport_err = EIO;
            goto done;
        }
    }

    if (parse_status_and_body(raw, raw_len, &status, &bptr, &blen) != 0) {
        out->transport_err = EPROTO;
        goto done;
    }
    if (blen > body_cap) {
        blen = body_cap;
    }
    if (blen > 0) {
        memcpy(body_buf, bptr, blen);
    }
    *body_len = blen;
    out->transport_err = 0;
    out->status = status;
    out->body = body_buf;
    out->body_len = blen;
    out->hdr_names = NULL;
    out->hdr_values = NULL;
    out->n_headers = 0;
    rc = 0;

done:
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    if (ssl_ctx) {
        SSL_CTX_free(ssl_ctx);
    }
    if (fd >= 0) {
        close(fd);
    }
    free(reqbuf);
    free(raw);
    if (rc != 0 && out->transport_err == 0) {
        out->transport_err = EIO;
        rc = 0;
    }
    return rc;
}
