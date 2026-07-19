/**
 * @file edge_tls.h
 * @brief OpenSSL non-blocking TLS helpers (P1.13 server / P1.13b client).
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

/** Client SSL_CTX (optional CA verify; NULL ca = no verify for lab). */
SSL_CTX *edge_tls_client_ctx_create(const char *ca_file);
void     edge_tls_client_ctx_free(SSL_CTX *ctx);

/** New SSL for accepted fd; SSL_set_accept_state. Free with SSL_free. */
SSL *edge_tls_conn_new_fd(SSL_CTX *ctx, int fd);

/** New client SSL for connected fd; SSL_set_connect_state + optional SNI. */
SSL *edge_tls_client_conn_new_fd(SSL_CTX *ctx, int fd, const char *sni_host);

/**
 * Drive handshake (server accept or client connect). Returns:
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

/**
 * Run handshake to completion on non-blocking fd using poll(2).
 * @return 0 ok, -1 error/timeout.
 */
int edge_tls_handshake_poll(SSL *ssl, int fd, int timeout_ms);

/**
 * Write all / read until EOF with poll on WANT_READ/WRITE.
 * @return 0 ok, -1 error.
 */
int edge_tls_write_all_poll(SSL *ssl, int fd, const void *buf, size_t len,
                            int timeout_ms);
int edge_tls_read_all_poll(SSL *ssl, int fd, void *buf, size_t cap,
                           size_t *out_len, int timeout_ms);

/** 1 if TLS configured (both cert and key non-empty). */
int edge_tls_config_enabled(const char *cert_file, const char *key_file);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_TLS_H */
