/**
 * @file tls_server.c
 * @brief OpenSSL non-blocking server + client helpers (P1.13 / P1.13b).
 */

#include "edge_tls.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

int edge_tls_config_enabled(const char *cert_file, const char *key_file)
{
    return cert_file && cert_file[0] && key_file && key_file[0];
}

SSL_CTX *edge_tls_ctx_create(const edge_tls_config_t *cfg)
{
    SSL_CTX *ctx;
    long mode;

    if (!cfg || !edge_tls_config_enabled(cfg->cert_file, cfg->key_file)) {
        return NULL;
    }

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
#endif

    ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        fprintf(stderr, "edgehost: SSL_CTX_new failed\n");
        ERR_print_errors_fp(stderr);
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    mode = SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER;
    SSL_CTX_set_mode(ctx, mode);

    if (SSL_CTX_use_certificate_chain_file(ctx, cfg->cert_file) != 1) {
        fprintf(stderr, "edgehost: failed to load TLS cert %s\n",
                cfg->cert_file);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, cfg->key_file, SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "edgehost: failed to load TLS key %s\n", cfg->key_file);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        fprintf(stderr, "edgehost: TLS cert/key mismatch\n");
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (cfg->client_ca_file && cfg->client_ca_file[0]) {
        if (SSL_CTX_load_verify_locations(ctx, cfg->client_ca_file, NULL) != 1) {
            fprintf(stderr, "edgehost: failed to load client CA %s\n",
                    cfg->client_ca_file);
            ERR_print_errors_fp(stderr);
            SSL_CTX_free(ctx);
            return NULL;
        }
        SSL_CTX_set_verify(ctx,
                           SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                           NULL);
    }

    return ctx;
}

void edge_tls_ctx_free(SSL_CTX *ctx)
{
    if (ctx) {
        SSL_CTX_free(ctx);
    }
}

SSL_CTX *edge_tls_client_ctx_create(const char *ca_file)
{
    SSL_CTX *ctx;

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_library_init();
    SSL_load_error_strings();
#endif
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                              SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    if (ca_file && ca_file[0]) {
        if (SSL_CTX_load_verify_locations(ctx, ca_file, NULL) != 1) {
            SSL_CTX_free(ctx);
            return NULL;
        }
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    } else {
        /* Lab / pinned upstream: no CA verify (same as prior outbound_http). */
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    }
    return ctx;
}

void edge_tls_client_ctx_free(SSL_CTX *ctx)
{
    edge_tls_ctx_free(ctx);
}

SSL *edge_tls_conn_new_fd(SSL_CTX *ctx, int fd)
{
    SSL *ssl;

    if (!ctx || fd < 0) {
        return NULL;
    }
    ssl = SSL_new(ctx);
    if (!ssl) {
        return NULL;
    }
    if (SSL_set_fd(ssl, fd) != 1) {
        SSL_free(ssl);
        return NULL;
    }
    SSL_set_accept_state(ssl);
    return ssl;
}

SSL *edge_tls_client_conn_new_fd(SSL_CTX *ctx, int fd, const char *sni_host)
{
    SSL *ssl;

    if (!ctx || fd < 0) {
        return NULL;
    }
    ssl = SSL_new(ctx);
    if (!ssl) {
        return NULL;
    }
    if (SSL_set_fd(ssl, fd) != 1) {
        SSL_free(ssl);
        return NULL;
    }
    SSL_set_connect_state(ssl);
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    if (sni_host && sni_host[0]) {
        (void)SSL_set_tlsext_host_name(ssl, sni_host);
    }
#else
    (void)sni_host;
#endif
    return ssl;
}

static int poll_fd(int fd, int want_write, int timeout_ms)
{
    struct pollfd pfd;
    int pr;

    pfd.fd = fd;
    pfd.events = want_write ? POLLOUT : POLLIN;
    pfd.revents = 0;
    pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) {
        return -1;
    }
    return 0;
}

int edge_tls_handshake_poll(SSL *ssl, int fd, int timeout_ms)
{
    for (;;) {
        int want_w = 0;
        int hr = edge_tls_handshake(ssl, &want_w);
        if (hr == 1) {
            return 0;
        }
        if (hr < 0) {
            return -1;
        }
        if (poll_fd(fd, want_w, timeout_ms) != 0) {
            return -1;
        }
    }
}

int edge_tls_write_all_poll(SSL *ssl, int fd, const void *buf, size_t len,
                            int timeout_ms)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        int want_w = 0;
        int n = edge_tls_write(ssl, p, len, &want_w);
        if (n > 0) {
            p += (size_t)n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0) {
            return -1;
        }
        if (poll_fd(fd, want_w, timeout_ms) != 0) {
            return -1;
        }
    }
    return 0;
}

int edge_tls_read_all_poll(SSL *ssl, int fd, void *buf, size_t cap,
                           size_t *out_len, int timeout_ms)
{
    uint8_t *p = (uint8_t *)buf;
    size_t off = 0;

    if (!out_len) {
        return -1;
    }
    *out_len = 0;
    while (off < cap) {
        int want_w = 0;
        int n = edge_tls_read(ssl, p + off, cap - off, &want_w);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n == -2) {
            break; /* EOF */
        }
        if (n < 0) {
            return -1;
        }
        /* WANT_READ/WRITE — if we already have data, stop; else wait */
        if (off > 0 && !want_w) {
            /* brief wait for more; if timeout, return what we have */
            if (poll_fd(fd, 0, 50) != 0) {
                break;
            }
            continue;
        }
        if (poll_fd(fd, want_w, timeout_ms) != 0) {
            if (off > 0) {
                break;
            }
            return -1;
        }
    }
    *out_len = off;
    return 0;
}

int edge_tls_handshake(SSL *ssl, int *want_write)
{
    int rc;
    int err;

    if (want_write) {
        *want_write = 0;
    }
    if (!ssl) {
        return -1;
    }
    rc = SSL_do_handshake(ssl);
    if (rc == 1) {
        return 1;
    }
    err = SSL_get_error(ssl, rc);
    if (err == SSL_ERROR_WANT_READ) {
        if (want_write) {
            *want_write = 0;
        }
        return 0;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        if (want_write) {
            *want_write = 1;
        }
        return 0;
    }
    return -1;
}

int edge_tls_read(SSL *ssl, void *buf, size_t len, int *want_write)
{
    int rc;
    int err;

    if (want_write) {
        *want_write = 0;
    }
    if (!ssl || !buf || len == 0) {
        return -1;
    }
    rc = SSL_read(ssl, buf, (int)len);
    if (rc > 0) {
        return rc;
    }
    err = SSL_get_error(ssl, rc);
    if (err == SSL_ERROR_WANT_READ) {
        return 0;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        if (want_write) {
            *want_write = 1;
        }
        return 0;
    }
    if (err == SSL_ERROR_ZERO_RETURN || rc == 0) {
        return -2;
    }
    return -1;
}

int edge_tls_write(SSL *ssl, const void *buf, size_t len, int *want_write)
{
    int rc;
    int err;

    if (want_write) {
        *want_write = 0;
    }
    if (!ssl || !buf) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    rc = SSL_write(ssl, buf, (int)len);
    if (rc > 0) {
        return rc;
    }
    err = SSL_get_error(ssl, rc);
    if (err == SSL_ERROR_WANT_READ) {
        if (want_write) {
            *want_write = 0;
        }
        return 0;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        if (want_write) {
            *want_write = 1;
        }
        return 0;
    }
    return -1;
}
