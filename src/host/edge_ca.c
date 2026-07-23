/**
 * @file edge_ca.c
 * @brief OpenSSL CA + Postgres storage + HTTP API.
 */

#define _GNU_SOURCE

#include "edge_ca.h"

#include "host_alloc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

struct edge_ca {
    int              enabled;
    edge_pg_config_t pg_cfg;
    edge_pg_conn_t  *pg;
    int              default_days;
    char             last_err[160];
};

static void set_err(edge_ca_t *ca, char *err, size_t err_sz, const char *msg)
{
    if (ca) {
        snprintf(ca->last_err, sizeof(ca->last_err), "%s", msg ? msg : "");
    }
    if (err && err_sz) {
        snprintf(err, err_sz, "%s", msg ? msg : "");
    }
}

static void openssl_err(edge_ca_t *ca, char *err, size_t err_sz, const char *pfx)
{
    char buf[128];
    unsigned long e = ERR_get_error();
    ERR_error_string_n(e, buf, sizeof(buf));
    if (err && err_sz) {
        snprintf(err, err_sz, "%s: %s", pfx ? pfx : "openssl", buf);
    }
    if (ca) {
        snprintf(ca->last_err, sizeof(ca->last_err), "%s: %s",
                 pfx ? pfx : "openssl", buf);
    }
}

edge_ca_t *edge_ca_create(const edge_config_t *cfg)
{
    edge_ca_t *ca;
    if (!cfg || !cfg->ca_enabled) {
        return NULL;
    }
    ca = (edge_ca_t *)host_alloc_kind(EDGE_MEM_OTHER, sizeof(*ca));
    if (!ca) {
        return NULL;
    }
    ca->enabled = 1;
    edge_pg_config_defaults(&ca->pg_cfg);
    if (cfg->ca_pg_sock[0]) {
        size_t n = strlen(cfg->ca_pg_sock);
        if (n >= sizeof(ca->pg_cfg.sock_path)) {
            n = sizeof(ca->pg_cfg.sock_path) - 1;
        }
        memcpy(ca->pg_cfg.sock_path, cfg->ca_pg_sock, n);
        ca->pg_cfg.sock_path[n] = '\0';
    }
    if (cfg->ca_pg_database[0]) {
        snprintf(ca->pg_cfg.database, sizeof(ca->pg_cfg.database), "%s",
                 cfg->ca_pg_database);
    }
    if (cfg->ca_pg_user[0]) {
        snprintf(ca->pg_cfg.user, sizeof(ca->pg_cfg.user), "%s",
                 cfg->ca_pg_user);
    }
    if (cfg->ca_pg_password[0]) {
        snprintf(ca->pg_cfg.password, sizeof(ca->pg_cfg.password), "%s",
                 cfg->ca_pg_password);
    }
    ca->pg_cfg.timeout_ms =
        cfg->ca_pg_timeout_ms ? cfg->ca_pg_timeout_ms : 5000;
    ca->default_days = cfg->ca_default_days > 0 ? cfg->ca_default_days : 825;
    ca->pg = edge_pg_connect(&ca->pg_cfg);
    if (!ca->pg) {
        fprintf(stderr,
                "edgehost: ca: postgres connect failed path=%s db=%s user=%s "
                "(will retry on use)\n",
                ca->pg_cfg.sock_path, ca->pg_cfg.database, ca->pg_cfg.user);
    }
    return ca;
}

void edge_ca_destroy(edge_ca_t *ca)
{
    if (!ca) {
        return;
    }
    edge_pg_close(ca->pg);
    host_free(ca);
}

int edge_ca_enabled(const edge_ca_t *ca)
{
    return ca && ca->enabled;
}

int edge_ca_ensure(edge_ca_t *ca)
{
    if (!ca) {
        return -1;
    }
    if (ca->pg) {
        return 0;
    }
    ca->pg = edge_pg_connect(&ca->pg_cfg);
    return ca->pg ? 0 : -1;
}

static char *pem_from_bio(BIO *bio)
{
    char *p = NULL;
    long n = BIO_get_mem_data(bio, &p);
    char *out;
    if (n <= 0 || !p) {
        return NULL;
    }
    out = (char *)host_alloc_kind(EDGE_MEM_OTHER, (size_t)n + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, p, (size_t)n);
    out[n] = '\0';
    return out;
}

static EVP_PKEY *gen_rsa_key(int bits)
{
    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!ctx) {
        return NULL;
    }
    if (EVP_PKEY_keygen_init(ctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) <= 0 ||
        EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return NULL;
    }
    EVP_PKEY_CTX_free(ctx);
    return pkey;
}

static int name_add(X509_NAME *name, const char *cn)
{
    if (!name || !cn || !cn[0]) {
        return -1;
    }
    if (!X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                    (const unsigned char *)cn, -1, -1, 0)) {
        return -1;
    }
    return 0;
}

static int x509_set_times(X509 *x, int days)
{
    if (!X509_gmtime_adj(X509_getm_notBefore(x), 0)) {
        return -1;
    }
    if (!X509_gmtime_adj(X509_getm_notAfter(x), (long)days * 24 * 3600)) {
        return -1;
    }
    return 0;
}

static int append_f(char *buf, size_t cap, size_t *off, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static int append_f(char *buf, size_t cap, size_t *off, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (*off >= cap) {
        return -1;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *off) {
        return -1;
    }
    *off += (size_t)n;
    return 0;
}

static int json_esc(const char *in, char *out, size_t out_sz)
{
    size_t o = 0, i;
    if (!out || out_sz < 3) {
        return -1;
    }
    out[o++] = '"';
    if (in) {
        for (i = 0; in[i] && o + 2 < out_sz; i++) {
            unsigned char c = (unsigned char)in[i];
            if (c == '"' || c == '\\') {
                out[o++] = '\\';
                out[o++] = (char)c;
            } else if (c == '\n') {
                out[o++] = '\\';
                out[o++] = 'n';
            } else if (c == '\r') {
                continue;
            } else if (c < 0x20) {
                continue;
            } else {
                out[o++] = (char)c;
            }
        }
    }
    if (o + 1 >= out_sz) {
        return -1;
    }
    out[o++] = '"';
    out[o] = '\0';
    return 0;
}

static int load_ca_pems(edge_ca_t *ca, int64_t ca_id, char *cert_pem,
                        size_t cert_sz, char *key_pem, size_t key_sz,
                        int64_t *serial_next, int64_t *crl_number)
{
    edge_pg_result_t r;
    char sql[256];
    const char *c, *k, *sn, *cn;
    if (edge_ca_ensure(ca) != 0) {
        return -1;
    }
    snprintf(sql, sizeof(sql),
             "SELECT cert_pem, key_pem, serial_next, crl_number FROM "
             "edgehost.ca_authority WHERE id=%lld AND active",
             (long long)ca_id);
    if (edge_pg_exec(ca->pg, sql, &r) != 0 || !r.ok || r.n_rows < 1) {
        edge_pg_result_clear(&r);
        return -1;
    }
    c = edge_pg_cell(&r, 0, 0);
    k = edge_pg_cell(&r, 0, 1);
    sn = edge_pg_cell(&r, 0, 2);
    cn = edge_pg_cell(&r, 0, 3);
    if (!c || !k) {
        edge_pg_result_clear(&r);
        return -1;
    }
    snprintf(cert_pem, cert_sz, "%s", c);
    snprintf(key_pem, key_sz, "%s", k);
    if (serial_next) {
        *serial_next = sn ? atoll(sn) : 2;
    }
    if (crl_number) {
        *crl_number = cn ? atoll(cn) : 1;
    }
    edge_pg_result_clear(&r);
    return 0;
}

static X509 *pem_to_x509(const char *pem)
{
    BIO *b = BIO_new_mem_buf(pem, -1);
    X509 *x;
    if (!b) {
        return NULL;
    }
    x = PEM_read_bio_X509(b, NULL, NULL, NULL);
    BIO_free(b);
    return x;
}

static EVP_PKEY *pem_to_key(const char *pem)
{
    BIO *b = BIO_new_mem_buf(pem, -1);
    EVP_PKEY *k;
    if (!b) {
        return NULL;
    }
    k = PEM_read_bio_PrivateKey(b, NULL, NULL, NULL);
    BIO_free(b);
    return k;
}

static int x509_to_pem(X509 *x, char *out, size_t out_sz)
{
    BIO *b = BIO_new(BIO_s_mem());
    char *p;
    long n;
    if (!b || PEM_write_bio_X509(b, x) != 1) {
        BIO_free(b);
        return -1;
    }
    n = BIO_get_mem_data(b, &p);
    if (n <= 0 || (size_t)n >= out_sz) {
        BIO_free(b);
        return -1;
    }
    memcpy(out, p, (size_t)n);
    out[n] = '\0';
    BIO_free(b);
    return 0;
}

static int key_to_pem(EVP_PKEY *k, char *out, size_t out_sz)
{
    BIO *b = BIO_new(BIO_s_mem());
    char *p;
    long n;
    if (!b || PEM_write_bio_PrivateKey(b, k, NULL, NULL, 0, NULL, NULL) != 1) {
        BIO_free(b);
        return -1;
    }
    n = BIO_get_mem_data(b, &p);
    if (n <= 0 || (size_t)n >= out_sz) {
        BIO_free(b);
        return -1;
    }
    memcpy(out, p, (size_t)n);
    out[n] = '\0';
    BIO_free(b);
    return 0;
}

int64_t edge_ca_create_authority(edge_ca_t *ca, const char *name, const char *cn,
                                 int days, char *err, size_t err_sz)
{
    EVP_PKEY *pkey = NULL;
    X509 *x = NULL;
    char cert_pem[EDGE_CA_PEM_MAX];
    char key_pem[EDGE_CA_PEM_MAX];
    char esc_name[256], esc_dn[EDGE_CA_DN_MAX * 2], esc_cert[EDGE_CA_PEM_MAX * 2],
        esc_key[EDGE_CA_PEM_MAX * 2];
    char sql[EDGE_CA_PEM_MAX * 5];
    edge_pg_result_t r;
    int64_t id = -1;
    BIGNUM *bn = NULL;
    char *subj = NULL;

    if (!ca || !name || !name[0] || !cn || !cn[0]) {
        set_err(ca, err, err_sz, "bad args");
        return -1;
    }
    if (days <= 0) {
        days = ca->default_days > 0 ? ca->default_days : 3650;
    }
    if (edge_ca_ensure(ca) != 0) {
        set_err(ca, err, err_sz, "postgres unavailable");
        return -1;
    }

    pkey = gen_rsa_key(2048);
    if (!pkey) {
        openssl_err(ca, err, err_sz, "keygen");
        return -1;
    }
    x = X509_new();
    if (!x) {
        EVP_PKEY_free(pkey);
        openssl_err(ca, err, err_sz, "X509_new");
        return -1;
    }
    X509_set_version(x, 2);
    bn = BN_new();
    BN_set_word(bn, 1);
    BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(x));
    BN_free(bn);
    if (x509_set_times(x, days) != 0 || X509_set_pubkey(x, pkey) != 1) {
        openssl_err(ca, err, err_sz, "set times/pubkey");
        goto fail;
    }
    {
        X509_NAME *nm = X509_get_subject_name(x);
        if (name_add(nm, cn) != 0 || X509_set_issuer_name(x, nm) != 1) {
            openssl_err(ca, err, err_sz, "set name");
            goto fail;
        }
    }
    X509_EXTENSION *ex;
    ex = X509V3_EXT_conf_nid(NULL, NULL, NID_basic_constraints, "critical,CA:TRUE");
    if (ex) {
        X509_add_ext(x, ex, -1);
        X509_EXTENSION_free(ex);
    }
    ex = X509V3_EXT_conf_nid(NULL, NULL, NID_key_usage,
                             "critical,keyCertSign,cRLSign");
    if (ex) {
        X509_add_ext(x, ex, -1);
        X509_EXTENSION_free(ex);
    }
    if (X509_sign(x, pkey, EVP_sha256()) == 0) {
        openssl_err(ca, err, err_sz, "sign CA");
        goto fail;
    }
    if (x509_to_pem(x, cert_pem, sizeof(cert_pem)) != 0 ||
        key_to_pem(pkey, key_pem, sizeof(key_pem)) != 0) {
        set_err(ca, err, err_sz, "pem encode");
        goto fail;
    }
    subj = X509_NAME_oneline(X509_get_subject_name(x), NULL, 0);
    if (edge_pg_escape_literal(name, esc_name, sizeof(esc_name)) != 0 ||
        edge_pg_escape_literal(subj ? subj : cn, esc_dn, sizeof(esc_dn)) != 0 ||
        edge_pg_escape_literal(cert_pem, esc_cert, sizeof(esc_cert)) != 0 ||
        edge_pg_escape_literal(key_pem, esc_key, sizeof(esc_key)) != 0) {
        set_err(ca, err, err_sz, "sql escape");
        goto fail;
    }
    snprintf(sql, sizeof(sql),
             "INSERT INTO edgehost.ca_authority "
             "(name, subject_dn, cert_pem, key_pem, serial_next, crl_number, active) "
             "VALUES (%s, %s, %s, %s, 2, 1, TRUE) RETURNING id",
             esc_name, esc_dn, esc_cert, esc_key);
    if (edge_pg_exec(ca->pg, sql, &r) != 0 || !r.ok || r.n_rows < 1) {
        set_err(ca, err, err_sz, r.err[0] ? r.err : "insert authority failed");
        edge_pg_result_clear(&r);
        goto fail;
    }
    id = atoll(edge_pg_cell(&r, 0, 0) ? edge_pg_cell(&r, 0, 0) : "0");
    edge_pg_result_clear(&r);
    OPENSSL_free(subj);
    X509_free(x);
    EVP_PKEY_free(pkey);
    return id;

fail:
    if (subj) {
        OPENSSL_free(subj);
    }
    X509_free(x);
    EVP_PKEY_free(pkey);
    return -1;
}

int64_t edge_ca_sign_csr(edge_ca_t *ca, int64_t ca_id, const char *csr_pem,
                         const char *device_id, int days, char *cert_out,
                         size_t cert_out_sz, char *err, size_t err_sz)
{
    char ca_cert_pem[EDGE_CA_PEM_MAX], ca_key_pem[EDGE_CA_PEM_MAX];
    int64_t serial = 0, crl_n = 0;
    X509 *ca_x = NULL, *leaf = NULL;
    EVP_PKEY *ca_key = NULL, *req_pub = NULL;
    X509_REQ *req = NULL;
    BIO *bio = NULL;
    char cert_pem[EDGE_CA_PEM_MAX];
    char esc_cert[EDGE_CA_PEM_MAX * 2], esc_csr[EDGE_CA_PEM_MAX * 2];
    char esc_dn[EDGE_CA_DN_MAX * 2], esc_cn[256], esc_dev[256];
    char sql[EDGE_CA_PEM_MAX * 5];
    edge_pg_result_t r;
    int64_t id = -1;
    char *subj = NULL;
    char cn_buf[EDGE_CA_NAME_MAX];
    BIGNUM *bn = NULL;
    time_t now = time(NULL);
    struct tm tm_b, tm_a;
    char nb[40], na[40];

    if (!ca || ca_id <= 0 || !csr_pem || !cert_out || cert_out_sz < 64) {
        set_err(ca, err, err_sz, "bad args");
        return -1;
    }
    if (days <= 0) {
        days = ca->default_days;
    }
    if (load_ca_pems(ca, ca_id, ca_cert_pem, sizeof(ca_cert_pem), ca_key_pem,
                     sizeof(ca_key_pem), &serial, &crl_n) != 0) {
        set_err(ca, err, err_sz, "load CA from postgres");
        return -1;
    }
    ca_x = pem_to_x509(ca_cert_pem);
    ca_key = pem_to_key(ca_key_pem);
    bio = BIO_new_mem_buf(csr_pem, -1);
    if (!ca_x || !ca_key || !bio) {
        openssl_err(ca, err, err_sz, "load CA/CSR");
        goto fail;
    }
    req = PEM_read_bio_X509_REQ(bio, NULL, NULL, NULL);
    if (!req || X509_REQ_verify(req, X509_REQ_get_pubkey(req)) <= 0) {
        openssl_err(ca, err, err_sz, "invalid CSR");
        goto fail;
    }
    req_pub = X509_REQ_get_pubkey(req);
    leaf = X509_new();
    if (!leaf || !req_pub) {
        openssl_err(ca, err, err_sz, "leaf alloc");
        goto fail;
    }
    X509_set_version(leaf, 2);
    bn = BN_new();
    BN_set_word(bn, (BN_ULONG)serial);
    BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(leaf));
    BN_free(bn);
    bn = NULL;
    if (x509_set_times(leaf, days) != 0 ||
        X509_set_pubkey(leaf, req_pub) != 1 ||
        X509_set_subject_name(leaf, X509_REQ_get_subject_name(req)) != 1 ||
        X509_set_issuer_name(leaf, X509_get_subject_name(ca_x)) != 1) {
        openssl_err(ca, err, err_sz, "leaf fields");
        goto fail;
    }
    {
        X509_EXTENSION *ex = X509V3_EXT_conf_nid(
            NULL, NULL, NID_basic_constraints, "critical,CA:FALSE");
        if (ex) {
            X509_add_ext(leaf, ex, -1);
            X509_EXTENSION_free(ex);
        }
        ex = X509V3_EXT_conf_nid(NULL, NULL, NID_key_usage,
                                 "digitalSignature,keyEncipherment");
        if (ex) {
            X509_add_ext(leaf, ex, -1);
            X509_EXTENSION_free(ex);
        }
        ex = X509V3_EXT_conf_nid(NULL, NULL, NID_ext_key_usage,
                                 "clientAuth,serverAuth");
        if (ex) {
            X509_add_ext(leaf, ex, -1);
            X509_EXTENSION_free(ex);
        }
    }
    if (X509_sign(leaf, ca_key, EVP_sha256()) == 0) {
        openssl_err(ca, err, err_sz, "sign leaf");
        goto fail;
    }
    if (x509_to_pem(leaf, cert_pem, sizeof(cert_pem)) != 0) {
        set_err(ca, err, err_sz, "pem leaf");
        goto fail;
    }
    snprintf(cert_out, cert_out_sz, "%s", cert_pem);
    subj = X509_NAME_oneline(X509_get_subject_name(leaf), NULL, 0);
    cn_buf[0] = '\0';
    X509_NAME_get_text_by_NID(X509_get_subject_name(leaf), NID_commonName,
                              cn_buf, (int)sizeof(cn_buf));
    gmtime_r(&now, &tm_b);
    {
        time_t exp = now + (time_t)days * 86400;
        gmtime_r(&exp, &tm_a);
    }
    strftime(nb, sizeof(nb), "%Y-%m-%d %H:%M:%S+00", &tm_b);
    strftime(na, sizeof(na), "%Y-%m-%d %H:%M:%S+00", &tm_a);

    if (edge_pg_escape_literal(cert_pem, esc_cert, sizeof(esc_cert)) != 0 ||
        edge_pg_escape_literal(csr_pem, esc_csr, sizeof(esc_csr)) != 0 ||
        edge_pg_escape_literal(subj ? subj : "", esc_dn, sizeof(esc_dn)) != 0 ||
        edge_pg_escape_literal(cn_buf, esc_cn, sizeof(esc_cn)) != 0 ||
        edge_pg_escape_literal(device_id ? device_id : "", esc_dev,
                               sizeof(esc_dev)) != 0) {
        set_err(ca, err, err_sz, "escape");
        goto fail;
    }
    snprintf(sql, sizeof(sql),
             "INSERT INTO edgehost.ca_certificate "
             "(ca_id, serial, subject_dn, common_name, device_id, not_before, "
             "not_after, cert_pem, csr_pem, status) VALUES "
             "(%lld, %lld, %s, %s, %s, '%s', '%s', %s, %s, 'valid') "
             "RETURNING id; "
             "UPDATE edgehost.ca_authority SET serial_next=%lld WHERE id=%lld",
             (long long)ca_id, (long long)serial, esc_dn, esc_cn, esc_dev, nb,
             na, esc_cert, esc_csr, (long long)(serial + 1), (long long)ca_id);
    /* simple query only one statement — split */
    snprintf(sql, sizeof(sql),
             "INSERT INTO edgehost.ca_certificate "
             "(ca_id, serial, subject_dn, common_name, device_id, not_before, "
             "not_after, cert_pem, csr_pem, status) VALUES "
             "(%lld, %lld, %s, %s, %s, TIMESTAMPTZ '%s', TIMESTAMPTZ '%s', %s, "
             "%s, 'valid') RETURNING id",
             (long long)ca_id, (long long)serial, esc_dn, esc_cn, esc_dev, nb,
             na, esc_cert, esc_csr);
    if (edge_pg_exec(ca->pg, sql, &r) != 0 || !r.ok || r.n_rows < 1) {
        set_err(ca, err, err_sz, r.err[0] ? r.err : "insert cert");
        edge_pg_result_clear(&r);
        goto fail;
    }
    id = atoll(edge_pg_cell(&r, 0, 0) ? edge_pg_cell(&r, 0, 0) : "0");
    edge_pg_result_clear(&r);
    snprintf(sql, sizeof(sql),
             "UPDATE edgehost.ca_authority SET serial_next=%lld WHERE id=%lld",
             (long long)(serial + 1), (long long)ca_id);
    (void)edge_pg_exec(ca->pg, sql, &r);
    edge_pg_result_clear(&r);

    OPENSSL_free(subj);
    X509_free(leaf);
    X509_REQ_free(req);
    EVP_PKEY_free(req_pub);
    EVP_PKEY_free(ca_key);
    X509_free(ca_x);
    BIO_free(bio);
    return id;

fail:
    if (subj) {
        OPENSSL_free(subj);
    }
    if (leaf) {
        X509_free(leaf);
    }
    if (req) {
        X509_REQ_free(req);
    }
    if (req_pub) {
        EVP_PKEY_free(req_pub);
    }
    if (ca_key) {
        EVP_PKEY_free(ca_key);
    }
    if (ca_x) {
        X509_free(ca_x);
    }
    if (bio) {
        BIO_free(bio);
    }
    return -1;
}

int64_t edge_ca_issue_leaf(edge_ca_t *ca, int64_t ca_id, const char *cn,
                           const char *device_id, int days, char *cert_out,
                           size_t cert_out_sz, char *key_out, size_t key_out_sz,
                           char *err, size_t err_sz)
{
    EVP_PKEY *pkey = NULL;
    X509_REQ *req = NULL;
    BIO *bio = NULL;
    char *csr_pem = NULL;
    int64_t id;

    if (!cn || !cn[0] || !key_out || key_out_sz < 64) {
        set_err(ca, err, err_sz, "bad args");
        return -1;
    }
    pkey = gen_rsa_key(2048);
    if (!pkey) {
        openssl_err(ca, err, err_sz, "keygen leaf");
        return -1;
    }
    req = X509_REQ_new();
    if (!req || X509_REQ_set_pubkey(req, pkey) != 1) {
        openssl_err(ca, err, err_sz, "req pubkey");
        goto fail;
    }
    {
        X509_NAME *nm = X509_REQ_get_subject_name(req);
        if (name_add(nm, cn) != 0 || X509_REQ_sign(req, pkey, EVP_sha256()) == 0) {
            openssl_err(ca, err, err_sz, "req sign");
            goto fail;
        }
    }
    bio = BIO_new(BIO_s_mem());
    if (!bio || PEM_write_bio_X509_REQ(bio, req) != 1) {
        openssl_err(ca, err, err_sz, "req pem");
        goto fail;
    }
    csr_pem = pem_from_bio(bio);
    if (!csr_pem || key_to_pem(pkey, key_out, key_out_sz) != 0) {
        set_err(ca, err, err_sz, "encode key");
        goto fail;
    }
    id = edge_ca_sign_csr(ca, ca_id, csr_pem, device_id, days, cert_out,
                          cert_out_sz, err, err_sz);
    host_free(csr_pem);
    BIO_free(bio);
    X509_REQ_free(req);
    EVP_PKEY_free(pkey);
    return id;

fail:
    host_free(csr_pem);
    if (bio) {
        BIO_free(bio);
    }
    if (req) {
        X509_REQ_free(req);
    }
    if (pkey) {
        EVP_PKEY_free(pkey);
    }
    return -1;
}

int edge_ca_rebuild_crl(edge_ca_t *ca, int64_t ca_id, char *crl_out,
                        size_t crl_out_sz, char *err, size_t err_sz)
{
    char ca_cert_pem[EDGE_CA_PEM_MAX], ca_key_pem[EDGE_CA_PEM_MAX];
    int64_t serial_next = 0, crl_n = 0;
    X509 *ca_x = NULL;
    EVP_PKEY *ca_key = NULL;
    X509_CRL *crl = NULL;
    edge_pg_result_t r;
    char *sql = NULL;
    size_t sql_cap = EDGE_CA_PEM_MAX * 3 + 512;
    int i;
    BIO *bio = NULL;
    char *pem = NULL;
    char *esc_pem = NULL;
    time_t now = time(NULL);
    struct tm tm_t, tm_n;
    char tu[40], nu[40];

    sql = (char *)host_alloc_kind(EDGE_MEM_OTHER, sql_cap);
    esc_pem = (char *)host_alloc_kind(EDGE_MEM_OTHER, EDGE_CA_PEM_MAX * 2);
    if (!sql || !esc_pem) {
        host_free(sql);
        host_free(esc_pem);
        set_err(ca, err, err_sz, "oom");
        return -1;
    }

    if (load_ca_pems(ca, ca_id, ca_cert_pem, sizeof(ca_cert_pem), ca_key_pem,
                     sizeof(ca_key_pem), &serial_next, &crl_n) != 0) {
        set_err(ca, err, err_sz, "load CA");
        return -1;
    }
    ca_x = pem_to_x509(ca_cert_pem);
    ca_key = pem_to_key(ca_key_pem);
    crl = X509_CRL_new();
    if (!ca_x || !ca_key || !crl) {
        openssl_err(ca, err, err_sz, "crl init");
        goto fail;
    }
    X509_CRL_set_version(crl, 1);
    X509_CRL_set_issuer_name(crl, X509_get_subject_name(ca_x));
    {
        ASN1_TIME *last = ASN1_TIME_new();
        ASN1_TIME *next = ASN1_TIME_new();
        if (!last || !next) {
            ASN1_TIME_free(last);
            ASN1_TIME_free(next);
            openssl_err(ca, err, err_sz, "crl times");
            goto fail;
        }
        X509_gmtime_adj(last, 0);
        X509_gmtime_adj(next, 7 * 24 * 3600);
        X509_CRL_set1_lastUpdate(crl, last);
        X509_CRL_set1_nextUpdate(crl, next);
        ASN1_TIME_free(last);
        ASN1_TIME_free(next);
    }

    snprintf(sql, sql_cap,
             "SELECT serial FROM edgehost.ca_certificate WHERE ca_id=%lld AND "
             "status='revoked'",
             (long long)ca_id);
    if (edge_pg_exec(ca->pg, sql, &r) != 0) {
        set_err(ca, err, err_sz, "list revoked");
        goto fail;
    }
    for (i = 0; i < r.n_rows; i++) {
        const char *s = edge_pg_cell(&r, i, 0);
        ASN1_INTEGER *ser;
        X509_REVOKED *rev;
        BIGNUM *bn;
        if (!s) {
            continue;
        }
        bn = BN_new();
        BN_set_word(bn, (BN_ULONG)atoll(s));
        ser = BN_to_ASN1_INTEGER(bn, NULL);
        BN_free(bn);
        rev = X509_REVOKED_new();
        X509_REVOKED_set_serialNumber(rev, ser);
        ASN1_INTEGER_free(ser);
        {
            ASN1_TIME *rt = ASN1_TIME_new();
            if (rt) {
                X509_gmtime_adj(rt, 0);
                X509_REVOKED_set_revocationDate(rev, rt);
                ASN1_TIME_free(rt);
            }
        }
        X509_CRL_add0_revoked(crl, rev);
    }
    edge_pg_result_clear(&r);

    if (X509_CRL_sign(crl, ca_key, EVP_sha256()) == 0) {
        openssl_err(ca, err, err_sz, "sign crl");
        goto fail;
    }
    bio = BIO_new(BIO_s_mem());
    if (!bio || PEM_write_bio_X509_CRL(bio, crl) != 1) {
        openssl_err(ca, err, err_sz, "crl pem");
        goto fail;
    }
    pem = pem_from_bio(bio);
    if (!pem) {
        set_err(ca, err, err_sz, "crl pem copy");
        goto fail;
    }
    if (crl_out && crl_out_sz) {
        snprintf(crl_out, crl_out_sz, "%s", pem);
    }
    gmtime_r(&now, &tm_t);
    {
        time_t nx = now + 7 * 86400;
        gmtime_r(&nx, &tm_n);
    }
    strftime(tu, sizeof(tu), "%Y-%m-%d %H:%M:%S+00", &tm_t);
    strftime(nu, sizeof(nu), "%Y-%m-%d %H:%M:%S+00", &tm_n);
    if (edge_pg_escape_literal(pem, esc_pem, EDGE_CA_PEM_MAX * 2) != 0) {
        set_err(ca, err, err_sz, "escape crl");
        goto fail;
    }
    if (snprintf(sql, sql_cap,
                 "INSERT INTO edgehost.ca_crl (ca_id, crl_number, crl_pem, "
                 "this_update, next_update) VALUES (%lld, %lld, %s, TIMESTAMPTZ "
                 "'%s', TIMESTAMPTZ '%s')",
                 (long long)ca_id, (long long)crl_n, esc_pem, tu,
                 nu) < 0 ||
        (size_t)strlen(sql) >= sql_cap) {
        set_err(ca, err, err_sz, "sql too long");
        goto fail;
    }
    if (edge_pg_exec(ca->pg, sql, &r) != 0 || !r.ok) {
        set_err(ca, err, err_sz, r.err[0] ? r.err : "insert crl");
        edge_pg_result_clear(&r);
        goto fail;
    }
    edge_pg_result_clear(&r);
    snprintf(sql, sql_cap,
             "UPDATE edgehost.ca_authority SET crl_number=%lld WHERE id=%lld",
             (long long)(crl_n + 1), (long long)ca_id);
    (void)edge_pg_exec(ca->pg, sql, &r);
    edge_pg_result_clear(&r);

    host_free(pem);
    host_free(esc_pem);
    host_free(sql);
    BIO_free(bio);
    X509_CRL_free(crl);
    EVP_PKEY_free(ca_key);
    X509_free(ca_x);
    return 0;

fail:
    host_free(pem);
    host_free(esc_pem);
    host_free(sql);
    if (bio) {
        BIO_free(bio);
    }
    if (crl) {
        X509_CRL_free(crl);
    }
    if (ca_key) {
        EVP_PKEY_free(ca_key);
    }
    if (ca_x) {
        X509_free(ca_x);
    }
    return -1;
}

int edge_ca_revoke(edge_ca_t *ca, int64_t cert_id, const char *reason,
                   char *err, size_t err_sz)
{
    edge_pg_result_t r;
    char sql[512];
    char esc_r[256];
    const char *ca_id_s;
    int64_t ca_id;

    if (edge_ca_ensure(ca) != 0) {
        set_err(ca, err, err_sz, "postgres unavailable");
        return -1;
    }
    if (edge_pg_escape_literal(reason ? reason : "unspecified", esc_r,
                               sizeof(esc_r)) != 0) {
        set_err(ca, err, err_sz, "escape");
        return -1;
    }
    snprintf(sql, sizeof(sql),
             "UPDATE edgehost.ca_certificate SET status='revoked', "
             "revoked_at=now(), revoke_reason=%s WHERE id=%lld AND "
             "status='valid' RETURNING ca_id",
             esc_r, (long long)cert_id);
    if (edge_pg_exec(ca->pg, sql, &r) != 0 || !r.ok || r.n_rows < 1) {
        set_err(ca, err, err_sz, r.err[0] ? r.err : "revoke failed");
        edge_pg_result_clear(&r);
        return -1;
    }
    ca_id_s = edge_pg_cell(&r, 0, 0);
    ca_id = ca_id_s ? atoll(ca_id_s) : 0;
    edge_pg_result_clear(&r);
    return edge_ca_rebuild_crl(ca, ca_id, NULL, 0, err, err_sz);
}

int edge_ca_get_crl_pem(edge_ca_t *ca, int64_t ca_id, char *crl_out,
                        size_t crl_out_sz)
{
    edge_pg_result_t r;
    char sql[384];
    const char *pem;
    if (!crl_out || crl_out_sz < 8 || edge_ca_ensure(ca) != 0) {
        return -1;
    }
    if (ca_id > 0) {
        snprintf(sql, sizeof(sql),
                 "SELECT crl_pem FROM edgehost.ca_crl WHERE ca_id=%lld ORDER BY "
                 "crl_number DESC LIMIT 1",
                 (long long)ca_id);
    } else {
        snprintf(sql, sizeof(sql),
                 "SELECT c.crl_pem FROM edgehost.ca_crl c "
                 "JOIN edgehost.ca_authority a ON a.id=c.ca_id AND a.active "
                 "ORDER BY c.id DESC LIMIT 1");
    }
    if (edge_pg_exec(ca->pg, sql, &r) != 0 || r.n_rows < 1) {
        edge_pg_result_clear(&r);
        return -1;
    }
    pem = edge_pg_cell(&r, 0, 0);
    if (!pem) {
        edge_pg_result_clear(&r);
        return -1;
    }
    snprintf(crl_out, crl_out_sz, "%s", pem);
    edge_pg_result_clear(&r);
    return 0;
}

int edge_ca_list_authorities_json(edge_ca_t *ca, char *buf, size_t buf_sz)
{
    edge_pg_result_t r;
    size_t off = 0;
    int i;
    if (!buf || edge_ca_ensure(ca) != 0) {
        return -1;
    }
    if (edge_pg_exec(ca->pg,
                     "SELECT id, name, subject_dn, serial_next, crl_number, "
                     "active::text FROM edgehost.ca_authority ORDER BY id",
                     &r) != 0) {
        edge_pg_result_clear(&r);
        return -1;
    }
    if (append_f(buf, buf_sz, &off, "{\"authorities\":[") != 0) {
        edge_pg_result_clear(&r);
        return -1;
    }
    for (i = 0; i < r.n_rows; i++) {
        char en[256], ed[EDGE_CA_DN_MAX * 2];
        const char *id = edge_pg_cell(&r, i, 0);
        const char *name = edge_pg_cell(&r, i, 1);
        const char *dn = edge_pg_cell(&r, i, 2);
        const char *sn = edge_pg_cell(&r, i, 3);
        const char *cn = edge_pg_cell(&r, i, 4);
        const char *act = edge_pg_cell(&r, i, 5);
        json_esc(name ? name : "", en, sizeof(en));
        json_esc(dn ? dn : "", ed, sizeof(ed));
        if (append_f(buf, buf_sz, &off,
                     "%s{\"id\":%s,\"name\":%s,\"subject_dn\":%s,"
                     "\"serial_next\":%s,\"crl_number\":%s,\"active\":%s}",
                     i ? "," : "", id ? id : "0", en, ed, sn ? sn : "0",
                     cn ? cn : "0",
                     (act && act[0] == 't') ? "true" : "false") != 0) {
            edge_pg_result_clear(&r);
            return -1;
        }
    }
    edge_pg_result_clear(&r);
    if (append_f(buf, buf_sz, &off, "]}") != 0) {
        return -1;
    }
    return (int)off;
}

int edge_ca_list_certs_json(edge_ca_t *ca, const char *status_filter, char *buf,
                            size_t buf_sz)
{
    edge_pg_result_t r;
    char sql[512];
    size_t off = 0;
    int i;
    if (!buf || edge_ca_ensure(ca) != 0) {
        return -1;
    }
    if (status_filter && status_filter[0]) {
        char esc[64];
        if (edge_pg_escape_literal(status_filter, esc, sizeof(esc)) != 0) {
            return -1;
        }
        snprintf(sql, sizeof(sql),
                 "SELECT id, ca_id, serial, subject_dn, common_name, "
                 "device_id, status, not_before::text, not_after::text "
                 "FROM edgehost.ca_certificate WHERE status=%s ORDER BY id DESC "
                 "LIMIT 200",
                 esc);
    } else {
        snprintf(sql, sizeof(sql),
                 "SELECT id, ca_id, serial, subject_dn, common_name, "
                 "device_id, status, not_before::text, not_after::text "
                 "FROM edgehost.ca_certificate ORDER BY id DESC LIMIT 200");
    }
    if (edge_pg_exec(ca->pg, sql, &r) != 0) {
        edge_pg_result_clear(&r);
        return -1;
    }
    if (append_f(buf, buf_sz, &off, "{\"certificates\":[") != 0) {
        edge_pg_result_clear(&r);
        return -1;
    }
    for (i = 0; i < r.n_rows; i++) {
        char edn[EDGE_CA_DN_MAX * 2], ecn[256], edv[256], est[32];
        char enb[96], ena[96];
        const char *id = edge_pg_cell(&r, i, 0);
        const char *caid = edge_pg_cell(&r, i, 1);
        const char *ser = edge_pg_cell(&r, i, 2);
        json_esc(edge_pg_cell(&r, i, 3), edn, sizeof(edn));
        json_esc(edge_pg_cell(&r, i, 4), ecn, sizeof(ecn));
        json_esc(edge_pg_cell(&r, i, 5), edv, sizeof(edv));
        json_esc(edge_pg_cell(&r, i, 6), est, sizeof(est));
        json_esc(edge_pg_cell(&r, i, 7), enb, sizeof(enb));
        json_esc(edge_pg_cell(&r, i, 8), ena, sizeof(ena));
        if (append_f(buf, buf_sz, &off,
                     "%s{\"id\":%s,\"ca_id\":%s,\"serial\":%s,"
                     "\"subject_dn\":%s,\"common_name\":%s,\"device_id\":%s,"
                     "\"status\":%s,\"not_before\":%s,\"not_after\":%s}",
                     i ? "," : "", id ? id : "0", caid ? caid : "0",
                     ser ? ser : "0", edn, ecn, edv, est, enb, ena) != 0) {
            edge_pg_result_clear(&r);
            return -1;
        }
    }
    edge_pg_result_clear(&r);
    if (append_f(buf, buf_sz, &off, "]}") != 0) {
        return -1;
    }
    return (int)off;
}

int edge_ca_get_cert_json(edge_ca_t *ca, int64_t cert_id, char *buf,
                          size_t buf_sz)
{
    edge_pg_result_t r;
    char sql[256];
    char edn[EDGE_CA_DN_MAX * 2], ecn[256], edv[256], est[32];
    char epem[EDGE_CA_PEM_MAX * 2], enb[96], ena[96];
    size_t off = 0;

    if (!buf || edge_ca_ensure(ca) != 0) {
        return -1;
    }
    snprintf(sql, sizeof(sql),
             "SELECT id, ca_id, serial, subject_dn, common_name, device_id, "
             "status, not_before::text, not_after::text, cert_pem "
             "FROM edgehost.ca_certificate WHERE id=%lld",
             (long long)cert_id);
    if (edge_pg_exec(ca->pg, sql, &r) != 0 || r.n_rows < 1) {
        edge_pg_result_clear(&r);
        return -1;
    }
    json_esc(edge_pg_cell(&r, 0, 3), edn, sizeof(edn));
    json_esc(edge_pg_cell(&r, 0, 4), ecn, sizeof(ecn));
    json_esc(edge_pg_cell(&r, 0, 5), edv, sizeof(edv));
    json_esc(edge_pg_cell(&r, 0, 6), est, sizeof(est));
    json_esc(edge_pg_cell(&r, 0, 7), enb, sizeof(enb));
    json_esc(edge_pg_cell(&r, 0, 8), ena, sizeof(ena));
    json_esc(edge_pg_cell(&r, 0, 9), epem, sizeof(epem));
    if (append_f(buf, buf_sz, &off,
                 "{\"id\":%s,\"ca_id\":%s,\"serial\":%s,\"subject_dn\":%s,"
                 "\"common_name\":%s,\"device_id\":%s,\"status\":%s,"
                 "\"not_before\":%s,\"not_after\":%s,\"cert_pem\":%s}",
                 edge_pg_cell(&r, 0, 0), edge_pg_cell(&r, 0, 1),
                 edge_pg_cell(&r, 0, 2), edn, ecn, edv, est, enb, ena,
                 epem) != 0) {
        edge_pg_result_clear(&r);
        return -1;
    }
    edge_pg_result_clear(&r);
    return (int)off;
}

int edge_ca_status_json(edge_ca_t *ca, char *buf, size_t buf_sz)
{
    edge_pg_result_t r;
    int n;
    const char *na = "0", *nc = "0", *nr = "0";
    if (!buf) {
        return -1;
    }
    if (!edge_ca_enabled(ca)) {
        n = snprintf(buf, buf_sz, "{\"enabled\":false}");
        return (n < 0 || (size_t)n >= buf_sz) ? -1 : n;
    }
    if (edge_ca_ensure(ca) != 0) {
        n = snprintf(buf, buf_sz,
                     "{\"enabled\":true,\"postgres\":false,\"error\":\"connect\"}");
        return (n < 0 || (size_t)n >= buf_sz) ? -1 : n;
    }
    if (edge_pg_exec(ca->pg,
                     "SELECT (SELECT count(*)::text FROM edgehost.ca_authority), "
                     "(SELECT count(*)::text FROM edgehost.ca_certificate), "
                     "(SELECT count(*)::text FROM edgehost.ca_certificate WHERE "
                     "status='revoked')",
                     &r) == 0 &&
        r.n_rows > 0) {
        na = edge_pg_cell(&r, 0, 0);
        nc = edge_pg_cell(&r, 0, 1);
        nr = edge_pg_cell(&r, 0, 2);
    }
    edge_pg_result_clear(&r);
    n = snprintf(buf, buf_sz,
                 "{\"enabled\":true,\"postgres\":true,"
                 "\"authorities\":%s,\"certificates\":%s,\"revoked\":%s}",
                 na ? na : "0", nc ? nc : "0", nr ? nr : "0");
    return (n < 0 || (size_t)n >= buf_sz) ? -1 : n;
}

static int http_json(char *out, size_t out_cap, size_t *out_len, int status,
                     const char *body)
{
    /* Caller builds full HTTP via edge_http1 — we only fill body + status. */
    size_t n;
    if (!out || !out_len || !body) {
        return -1;
    }
    n = strlen(body);
    if (n >= out_cap) {
        return -1;
    }
    memcpy(out, body, n + 1);
    *out_len = n;
    (void)status;
    return 0;
}

static int json_field(const char *body, size_t len, const char *key, char *out,
                      size_t out_sz)
{
    char pat[96];
    const char *p;
    size_t i = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = body;
    while (p && (size_t)(p - body) < len) {
        const char *f = strstr(p, pat);
        const char *q;
        if (!f) {
            return -1;
        }
        q = f + strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == ':') {
            q++;
        }
        if (*q == '"') {
            q++;
            while (*q && *q != '"' && i + 1 < out_sz) {
                if (*q == '\\' && q[1]) {
                    q++;
                }
                out[i++] = *q++;
            }
            out[i] = '\0';
            return 0;
        }
        /* number */
        while (*q && ((*q >= '0' && *q <= '9') || *q == '-') && i + 1 < out_sz) {
            out[i++] = *q++;
        }
        out[i] = '\0';
        return i > 0 ? 0 : -1;
    }
    return -1;
}

static int build_http_response(char *out, size_t out_cap, size_t *out_len,
                               int status, const char *reason, const char *ctype,
                               const char *body, size_t body_len)
{
    int n;
    if (!out || !out_len) {
        return -1;
    }
    n = snprintf(out, out_cap,
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 status, reason ? reason : "OK",
                 ctype ? ctype : "application/json", body_len);
    if (n < 0 || (size_t)n >= out_cap) {
        return -1;
    }
    if ((size_t)n + body_len >= out_cap) {
        return -1;
    }
    memcpy(out + n, body, body_len);
    out[n + body_len] = '\0';
    *out_len = (size_t)n + body_len;
    return 0;
}

int edge_ca_http_dispatch(edge_ca_t *ca, const char *method, const char *path,
                          const uint8_t *body, size_t body_len, char *out,
                          size_t out_cap, size_t *out_len, int *http_status)
{
    static char jbuf[EDGE_CA_JSON_MAX];
    static char pem[EDGE_CA_PEM_MAX];
    char err[160];
    int jn;
    const char *p;
    int public_crl = 0;

    if (!method || !path || !out || !out_len || !http_status) {
        return -1;
    }
    *http_status = 500;

    /* Public CRL (no auth at this layer — caller may still gate). */
    if ((strcmp(path, "/ca/crl.pem") == 0 ||
         strcmp(path, "/api/v1/ca/crl.pem") == 0) &&
        strcmp(method, "GET") == 0) {
        public_crl = 1;
        if (!edge_ca_enabled(ca) ||
            edge_ca_get_crl_pem(ca, 0, pem, sizeof(pem)) != 0) {
            const char *nb = "CRL not available\n";
            *http_status = 404;
            return build_http_response(out, out_cap, out_len, 404, "Not Found",
                                       "text/plain", nb, strlen(nb));
        }
        *http_status = 200;
        return build_http_response(out, out_cap, out_len, 200, "OK",
                                   "application/pkix-crl", pem, strlen(pem));
    }

    if (strncmp(path, "/api/v1/ca", 10) != 0) {
        return 0;
    }
    p = path + 10;
    if (*p == '?') {
        return 0;
    }
    if (*p != '\0' && *p != '/') {
        return 0;
    }
    if (*p == '/') {
        p++;
    }

    if (!edge_ca_enabled(ca)) {
        const char *b = "{\"error\":\"CA_DISABLED\"}";
        *http_status = 503;
        return build_http_response(out, out_cap, out_len, 503,
                                   "Service Unavailable", "application/json", b,
                                   strlen(b));
    }

    if ((p[0] == '\0' || strcmp(p, "status") == 0) &&
        strcmp(method, "GET") == 0) {
        jn = edge_ca_status_json(ca, jbuf, sizeof(jbuf));
        if (jn < 0) {
            return -1;
        }
        *http_status = 200;
        return build_http_response(out, out_cap, out_len, 200, "OK",
                                   "application/json", jbuf, (size_t)jn);
    }

    if (strcmp(p, "authorities") == 0 && strcmp(method, "GET") == 0) {
        jn = edge_ca_list_authorities_json(ca, jbuf, sizeof(jbuf));
        if (jn < 0) {
            const char *b = "{\"error\":\"LIST_FAIL\"}";
            *http_status = 500;
            return build_http_response(out, out_cap, out_len, 500, "Error",
                                       "application/json", b, strlen(b));
        }
        *http_status = 200;
        return build_http_response(out, out_cap, out_len, 200, "OK",
                                   "application/json", jbuf, (size_t)jn);
    }

    if (strcmp(p, "authorities") == 0 && strcmp(method, "POST") == 0) {
        char name[64], cn[128], days_s[16];
        int days = 3650;
        int64_t id;
        name[0] = cn[0] = '\0';
        (void)json_field((const char *)body, body_len, "name", name,
                         sizeof(name));
        (void)json_field((const char *)body, body_len, "cn", cn, sizeof(cn));
        if (json_field((const char *)body, body_len, "days", days_s,
                       sizeof(days_s)) == 0) {
            days = atoi(days_s);
        }
        if (!name[0] || !cn[0]) {
            const char *b = "{\"error\":\"name and cn required\"}";
            *http_status = 400;
            return build_http_response(out, out_cap, out_len, 400, "Bad Request",
                                       "application/json", b, strlen(b));
        }
        id = edge_ca_create_authority(ca, name, cn, days, err, sizeof(err));
        if (id <= 0) {
            jn = snprintf(jbuf, sizeof(jbuf), "{\"ok\":false,\"error\":\"%s\"}",
                          err[0] ? err : "create failed");
            *http_status = 500;
            return build_http_response(out, out_cap, out_len, 500, "Error",
                                       "application/json", jbuf, (size_t)jn);
        }
        (void)edge_ca_rebuild_crl(ca, id, NULL, 0, err, sizeof(err));
        jn = snprintf(jbuf, sizeof(jbuf), "{\"ok\":true,\"id\":%lld}",
                      (long long)id);
        *http_status = 201;
        return build_http_response(out, out_cap, out_len, 201, "Created",
                                   "application/json", jbuf, (size_t)jn);
    }

    if (strncmp(p, "certs", 5) == 0 && strcmp(method, "GET") == 0) {
        const char *q = strchr(path, '?');
        char status_f[32];
        status_f[0] = '\0';
        if (q && strstr(q, "status=")) {
            const char *s = strstr(q, "status=") + 7;
            size_t i = 0;
            while (*s && *s != '&' && i + 1 < sizeof(status_f)) {
                status_f[i++] = *s++;
            }
            status_f[i] = '\0';
        }
        if (p[5] == '/' && p[6] >= '0' && p[6] <= '9') {
            int64_t id = atoll(p + 6);
            jn = edge_ca_get_cert_json(ca, id, jbuf, sizeof(jbuf));
            if (jn < 0) {
                const char *b = "{\"error\":\"NOT_FOUND\"}";
                *http_status = 404;
                return build_http_response(out, out_cap, out_len, 404,
                                           "Not Found", "application/json", b,
                                           strlen(b));
            }
            *http_status = 200;
            return build_http_response(out, out_cap, out_len, 200, "OK",
                                       "application/json", jbuf, (size_t)jn);
        }
        jn = edge_ca_list_certs_json(ca, status_f[0] ? status_f : NULL, jbuf,
                                     sizeof(jbuf));
        if (jn < 0) {
            const char *b = "{\"error\":\"LIST_FAIL\"}";
            *http_status = 500;
            return build_http_response(out, out_cap, out_len, 500, "Error",
                                       "application/json", b, strlen(b));
        }
        *http_status = 200;
        return build_http_response(out, out_cap, out_len, 200, "OK",
                                   "application/json", jbuf, (size_t)jn);
    }

    if (strcmp(p, "sign") == 0 && strcmp(method, "POST") == 0) {
        char csr[EDGE_CA_PEM_MAX], device_id[128], ca_id_s[16], days_s[16];
        int64_t ca_id = 1, id;
        int days = 0;
        char cert[EDGE_CA_PEM_MAX];
        char ec[EDGE_CA_PEM_MAX * 2];
        csr[0] = device_id[0] = '\0';
        (void)json_field((const char *)body, body_len, "csr_pem", csr,
                         sizeof(csr));
        (void)json_field((const char *)body, body_len, "device_id", device_id,
                         sizeof(device_id));
        if (json_field((const char *)body, body_len, "ca_id", ca_id_s,
                       sizeof(ca_id_s)) == 0) {
            ca_id = atoll(ca_id_s);
        }
        if (json_field((const char *)body, body_len, "days", days_s,
                       sizeof(days_s)) == 0) {
            days = atoi(days_s);
        }
        /* Also accept raw PEM body */
        if (!csr[0] && body_len > 10 &&
            strstr((const char *)body, "BEGIN CERTIFICATE REQUEST")) {
            size_t n = body_len < sizeof(csr) - 1 ? body_len : sizeof(csr) - 1;
            memcpy(csr, body, n);
            csr[n] = '\0';
        }
        if (!csr[0]) {
            const char *b = "{\"error\":\"csr_pem required\"}";
            *http_status = 400;
            return build_http_response(out, out_cap, out_len, 400, "Bad Request",
                                       "application/json", b, strlen(b));
        }
        id = edge_ca_sign_csr(ca, ca_id, csr, device_id, days, cert, sizeof(cert),
                              err, sizeof(err));
        if (id <= 0) {
            jn = snprintf(jbuf, sizeof(jbuf), "{\"ok\":false,\"error\":\"%s\"}",
                          err[0] ? err : "sign failed");
            *http_status = 400;
            return build_http_response(out, out_cap, out_len, 400, "Bad Request",
                                       "application/json", jbuf, (size_t)jn);
        }
        json_esc(cert, ec, sizeof(ec));
        jn = snprintf(jbuf, sizeof(jbuf),
                      "{\"ok\":true,\"id\":%lld,\"cert_pem\":%s}", (long long)id,
                      ec);
        *http_status = 201;
        return build_http_response(out, out_cap, out_len, 201, "Created",
                                   "application/json", jbuf, (size_t)jn);
    }

    if (strcmp(p, "issue") == 0 && strcmp(method, "POST") == 0) {
        char cn[128], device_id[128], ca_id_s[16], days_s[16];
        int64_t ca_id = 1, id;
        int days = 0;
        char cert[EDGE_CA_PEM_MAX], key[EDGE_CA_PEM_MAX];
        char ec[EDGE_CA_PEM_MAX * 2], ek[EDGE_CA_PEM_MAX * 2];
        cn[0] = device_id[0] = '\0';
        (void)json_field((const char *)body, body_len, "cn", cn, sizeof(cn));
        (void)json_field((const char *)body, body_len, "device_id", device_id,
                         sizeof(device_id));
        if (json_field((const char *)body, body_len, "ca_id", ca_id_s,
                       sizeof(ca_id_s)) == 0) {
            ca_id = atoll(ca_id_s);
        }
        if (json_field((const char *)body, body_len, "days", days_s,
                       sizeof(days_s)) == 0) {
            days = atoi(days_s);
        }
        if (!cn[0]) {
            const char *b = "{\"error\":\"cn required\"}";
            *http_status = 400;
            return build_http_response(out, out_cap, out_len, 400, "Bad Request",
                                       "application/json", b, strlen(b));
        }
        id = edge_ca_issue_leaf(ca, ca_id, cn, device_id, days, cert,
                                sizeof(cert), key, sizeof(key), err, sizeof(err));
        if (id <= 0) {
            jn = snprintf(jbuf, sizeof(jbuf), "{\"ok\":false,\"error\":\"%s\"}",
                          err[0] ? err : "issue failed");
            *http_status = 400;
            return build_http_response(out, out_cap, out_len, 400, "Bad Request",
                                       "application/json", jbuf, (size_t)jn);
        }
        json_esc(cert, ec, sizeof(ec));
        json_esc(key, ek, sizeof(ek));
        jn = snprintf(jbuf, sizeof(jbuf),
                      "{\"ok\":true,\"id\":%lld,\"cert_pem\":%s,\"key_pem\":%s}",
                      (long long)id, ec, ek);
        *http_status = 201;
        return build_http_response(out, out_cap, out_len, 201, "Created",
                                   "application/json", jbuf, (size_t)jn);
    }

    if (strncmp(p, "certs/", 6) == 0 && strstr(p, "/revoke") &&
        strcmp(method, "POST") == 0) {
        int64_t id = atoll(p + 6);
        char reason[64];
        reason[0] = '\0';
        (void)json_field((const char *)body, body_len, "reason", reason,
                         sizeof(reason));
        if (edge_ca_revoke(ca, id, reason[0] ? reason : "unspecified", err,
                           sizeof(err)) != 0) {
            jn = snprintf(jbuf, sizeof(jbuf), "{\"ok\":false,\"error\":\"%s\"}",
                          err[0] ? err : "revoke failed");
            *http_status = 400;
            return build_http_response(out, out_cap, out_len, 400, "Bad Request",
                                       "application/json", jbuf, (size_t)jn);
        }
        jn = snprintf(jbuf, sizeof(jbuf), "{\"ok\":true,\"id\":%lld}",
                      (long long)id);
        *http_status = 200;
        return build_http_response(out, out_cap, out_len, 200, "OK",
                                   "application/json", jbuf, (size_t)jn);
    }

    if (strcmp(p, "crl/rebuild") == 0 && strcmp(method, "POST") == 0) {
        char ca_id_s[16];
        int64_t ca_id = 1;
        if (json_field((const char *)body, body_len, "ca_id", ca_id_s,
                       sizeof(ca_id_s)) == 0) {
            ca_id = atoll(ca_id_s);
        }
        if (edge_ca_rebuild_crl(ca, ca_id, pem, sizeof(pem), err, sizeof(err)) !=
            0) {
            jn = snprintf(jbuf, sizeof(jbuf), "{\"ok\":false,\"error\":\"%s\"}",
                          err[0] ? err : "crl failed");
            *http_status = 500;
            return build_http_response(out, out_cap, out_len, 500, "Error",
                                       "application/json", jbuf, (size_t)jn);
        }
        jn = snprintf(jbuf, sizeof(jbuf), "{\"ok\":true,\"ca_id\":%lld}",
                      (long long)ca_id);
        *http_status = 200;
        return build_http_response(out, out_cap, out_len, 200, "OK",
                                   "application/json", jbuf, (size_t)jn);
    }

    (void)public_crl;
    (void)http_json;
    {
        const char *b = "{\"error\":\"NOT_FOUND\"}";
        *http_status = 404;
        return build_http_response(out, out_cap, out_len, 404, "Not Found",
                                   "application/json", b, strlen(b));
    }
}
