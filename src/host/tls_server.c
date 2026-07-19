/**
 * @file tls_server.c
 * @brief OpenSSL non-blocking server context + handshake helpers (P1.13).
 */

#include "edge_tls.h"

#include <stdio.h>
#include <string.h>

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
