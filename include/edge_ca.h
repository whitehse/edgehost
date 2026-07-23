/**
 * @file edge_ca.h
 * @brief OpenSSL certificate authority backed by Postgres (Unix socket).
 *
 * Signing uses OpenSSL X509 APIs on the host thread. Postgres I/O uses the
 * non-blocking edge_pg client (poll). CRL PEM is published for HTTP serve.
 */
#ifndef EDGE_CA_H
#define EDGE_CA_H

#include "edge_config.h"
#include "edge_pg.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGE_CA_PEM_MAX      (24 * 1024)
#define EDGE_CA_DN_MAX       512
#define EDGE_CA_NAME_MAX     128
#define EDGE_CA_JSON_MAX     (64 * 1024)

typedef struct edge_ca edge_ca_t;

typedef struct {
    int64_t id;
    char    name[EDGE_CA_NAME_MAX];
    char    subject_dn[EDGE_CA_DN_MAX];
    int64_t serial_next;
    int64_t crl_number;
    int     active;
} edge_ca_authority_info_t;

typedef struct {
    int64_t id;
    int64_t ca_id;
    int64_t serial;
    char    subject_dn[EDGE_CA_DN_MAX];
    char    common_name[EDGE_CA_NAME_MAX];
    char    device_id[EDGE_CA_NAME_MAX];
    char    status[16];
    char    not_before[40];
    char    not_after[40];
} edge_ca_cert_info_t;

edge_ca_t *edge_ca_create(const edge_config_t *cfg);
void       edge_ca_destroy(edge_ca_t *ca);
int        edge_ca_enabled(const edge_ca_t *ca);

/** Reconnect if needed. @return 0 ok, -1 fail. */
int edge_ca_ensure(edge_ca_t *ca);

/**
 * Create a self-signed CA (RSA/EC) and store PEMs in Postgres.
 * @p name unique label; @p cn used in subject; @p days validity.
 * @return new ca_id >0, or -1.
 */
int64_t edge_ca_create_authority(edge_ca_t *ca, const char *name,
                                 const char *cn, int days, char *err,
                                 size_t err_sz);

/**
 * Sign a PEM CSR; store issued cert. Optional device_id annotation.
 * Writes cert PEM to @p cert_out.
 * @return cert id >0, or -1.
 */
int64_t edge_ca_sign_csr(edge_ca_t *ca, int64_t ca_id, const char *csr_pem,
                         const char *device_id, int days, char *cert_out,
                         size_t cert_out_sz, char *err, size_t err_sz);

/**
 * Issue a leaf cert with generated key (lab convenience). Returns cert+key PEM.
 */
int64_t edge_ca_issue_leaf(edge_ca_t *ca, int64_t ca_id, const char *cn,
                           const char *device_id, int days, char *cert_out,
                           size_t cert_out_sz, char *key_out, size_t key_out_sz,
                           char *err, size_t err_sz);

/** Revoke by certificate id; rebuilds CRL. */
int edge_ca_revoke(edge_ca_t *ca, int64_t cert_id, const char *reason,
                   char *err, size_t err_sz);

/** Rebuild and store CRL for CA; PEM in @p crl_out. */
int edge_ca_rebuild_crl(edge_ca_t *ca, int64_t ca_id, char *crl_out,
                        size_t crl_out_sz, char *err, size_t err_sz);

/** Latest CRL PEM for active CA (or ca_id if >0). */
int edge_ca_get_crl_pem(edge_ca_t *ca, int64_t ca_id, char *crl_out,
                        size_t crl_out_sz);

/** JSON list authorities. */
int edge_ca_list_authorities_json(edge_ca_t *ca, char *buf, size_t buf_sz);

/** JSON list certificates (optional status filter). */
int edge_ca_list_certs_json(edge_ca_t *ca, const char *status_filter, char *buf,
                            size_t buf_sz);

/** Single cert JSON including PEM. */
int edge_ca_get_cert_json(edge_ca_t *ca, int64_t cert_id, char *buf,
                          size_t buf_sz);

/** CA status JSON for /api/v1/ca/status. */
int edge_ca_status_json(edge_ca_t *ca, char *buf, size_t buf_sz);

/**
 * HTTP dispatch for /api/v1/ca/ routes and public /ca/crl.pem.
 * @return 1 handled, 0 not this path, -1 hard error.
 */
int edge_ca_http_dispatch(edge_ca_t *ca, const char *method, const char *path,
                          const uint8_t *body, size_t body_len, char *out,
                          size_t out_cap, size_t *out_len, int *http_status);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_CA_H */
