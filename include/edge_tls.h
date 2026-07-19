/**
 * @file edge_tls.h
 * @brief OpenSSL non-blocking server helpers (P1.13 / ADR-014 TLS).
 *
 * edgehost uses OpenSSL with SSL_set_fd + want-read/write (io_uring POLL).
 * CPE agent uses mbedTLS separately — not linked here.
 */
#ifndef EDGE_TLS_H
#define EDGE_TLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st     SSL;

typedef struct {
    const char *cert_file;
    const char *key_file;
    /** Optional client CA for mTLS (NULL = no client cert required). */
    const char *client_ca_file;
} edge_tls_config_t;

/**
 * Create server SSL_CTX. Returns NULL on error (message on stderr).
 * Caller frees with edge_tls_ctx_free.
 */
SSL_CTX *edge_tls_ctx_create(const edge_tls_config_t *cfg);
void     edge_tls_ctx_free(SSL_CTX *ctx);

/** New SSL for accepted fd; SSL_set_accept_state. Free with SSL_free. */
SSL *edge_tls_conn_new_fd(SSL_CTX *ctx, int fd);

/**
 * Drive handshake. Returns:
 *   1 complete, 0 want I/O (*want_write=1 if need POLLOUT else POLLIN),
 *  -1 fatal error.
 */
int edge_tls_handshake(SSL *ssl, int *want_write);

/**
 * SSL_read wrapper. Returns:
 *   >0 bytes, 0 want I/O (*want_write), -1 error, -2 clean EOF.
 */
int edge_tls_read(SSL *ssl, void *buf, size_t len, int *want_write);

/**
 * SSL_write wrapper. Returns:
 *   >0 bytes written, 0 want I/O (*want_write), -1 error.
 */
int edge_tls_write(SSL *ssl, const void *buf, size_t len, int *want_write);

/** 1 if TLS configured (both cert and key non-empty). */
int edge_tls_config_enabled(const char *cert_file, const char *key_file);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_TLS_H */
