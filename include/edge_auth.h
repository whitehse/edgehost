/**
 * @file edge_auth.h
 * @brief Principals, RBAC, lab sessions (P1.7c), proxy header HMAC (P1.7d).
 *
 * Syscall-free. Host loads secrets from env and attaches an edge_auth_ctx_t
 * to HTTP serve. Mode OPEN leaves APIs open (default for unit tests).
 */
#ifndef EDGE_AUTH_H
#define EDGE_AUTH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGE_AUTH_SUB_MAX       64
#define EDGE_AUTH_COOKIE_NAME   "edge_session"
#define EDGE_AUTH_COOKIE_MAX    768
#define EDGE_AUTH_PASSWORD_MAX  128
#define EDGE_AUTH_HMAC_KEY_MAX  64
#define EDGE_AUTH_ENV_NAME_MAX  64
#define EDGE_AUTH_ROLES_MAX     8
#define EDGE_AUTH_ROUTER_IDS    8
#define EDGE_AUTH_ROLES_CSV_MAX 128
#define EDGE_AUTH_PROXY_SIG_MAX 64

/** Proxy injects these headers (P1.7d); signature covers canonical form. */
#define EDGE_AUTH_HDR_USER      "X-User"
#define EDGE_AUTH_HDR_ROLES     "X-Roles"
#define EDGE_AUTH_HDR_TS        "X-Auth-Timestamp"
#define EDGE_AUTH_HDR_SIG       "X-Auth-Signature"

typedef enum {
    EDGE_AUTH_MODE_OPEN = 0,         /* no checks (tests / explicit open lab) */
    EDGE_AUTH_MODE_LAB_PASSWORD = 1, /* P1.7c step 1 */
    EDGE_AUTH_MODE_PROXY_HEADERS = 2 /* P1.7d: reverse-proxy X-User + HMAC */
} edge_auth_mode_t;

/** Role bit flags (stable across authn mechanisms). */
typedef enum {
    EDGE_ROLE_NONE            = 0,
    EDGE_ROLE_EMPLOYEE        = 1u << 0,
    EDGE_ROLE_EMPLOYEE_ADMIN  = 1u << 1,
    EDGE_ROLE_CPE             = 1u << 2,
    EDGE_ROLE_CUSTOMER        = 1u << 3,
    EDGE_ROLE_SERVICE_OPENAI  = 1u << 4,
    EDGE_ROLE_INGEST          = 1u << 5
} edge_role_t;

typedef enum {
    EDGE_AUTH_ALLOW = 0,
    EDGE_AUTH_DENY  = 1
} edge_auth_decision_t;

typedef enum {
    EDGE_RES_NONE = 0,
    EDGE_RES_STATE_GET,
    EDGE_RES_STATE_PUT,
    EDGE_RES_STATE_DELETE,
    EDGE_RES_STATE_LIST,
    EDGE_RES_WS_STREAM,
    EDGE_RES_PACKAGES
} edge_auth_resource_t;

typedef struct {
    int      authenticated; /* 1 if session verified */
    char     sub[EDGE_AUTH_SUB_MAX];
    uint32_t roles; /* edge_role_t bits */
    int64_t  exp;   /* unix seconds; 0 = no expiry check if unauth */
    char     router_ids[EDGE_AUTH_ROUTER_IDS][EDGE_AUTH_SUB_MAX];
    int      n_router_ids;
} edge_principal_t;

/**
 * Runtime auth context (host fills secrets; not owned by edgecore).
 * LAB_PASSWORD: lab_password + hmac_key (session cookie).
 * PROXY_HEADERS: proxy_hmac_key for X-Auth-Signature.
 */
typedef struct {
    edge_auth_mode_t mode;
    char             lab_password[EDGE_AUTH_PASSWORD_MAX];
    uint8_t          hmac_key[EDGE_AUTH_HMAC_KEY_MAX];
    size_t           hmac_key_len;
    uint8_t          proxy_hmac_key[EDGE_AUTH_HMAC_KEY_MAX];
    size_t           proxy_hmac_key_len;
    uint32_t         session_ttl_s;    /* default 28800 */
    uint32_t         proxy_max_skew_s; /* default 300; timestamp window */
    /**
     * Clock for exp/skew checks. If now_sec_override > 0, use it; else time(NULL).
     */
    int64_t          now_sec_override;
} edge_auth_ctx_t;

void edge_auth_ctx_init(edge_auth_ctx_t *ctx);
void edge_principal_clear(edge_principal_t *p);

int edge_auth_role_has(uint32_t roles, edge_role_t role);
const char *edge_auth_mode_name(edge_auth_mode_t m);
const char *edge_auth_resource_name(edge_auth_resource_t r);

/** Parse role name → bit; 0 if unknown. */
uint32_t edge_auth_role_parse(const char *name);

/**
 * Pure RBAC. Anonymous (authenticated==0) denied for all listed resources.
 * employee / employee_admin: full state + stream + packages.
 * ingest: STATE_PUT only.
 */
edge_auth_decision_t edge_auth_rbac_check(const edge_principal_t *p,
                                          edge_auth_resource_t res,
                                          const char *ns, const char *key);

/** Current time (seconds). Uses override if set, else time(NULL). */
int64_t edge_auth_now_sec(const edge_auth_ctx_t *ctx);

/**
 * Issue lab employee session cookie value (payload.sig).
 * @return 0 ok, -1 error.
 */
int edge_auth_session_issue(const edge_auth_ctx_t *ctx, const char *sub,
                            uint32_t roles, char *cookie_val,
                            size_t cookie_val_sz, edge_principal_t *out_p);

/**
 * Verify cookie value into principal (checks HMAC + exp).
 * @return 0 ok (principal filled), -1 invalid/expired.
 */
int edge_auth_session_verify(const edge_auth_ctx_t *ctx,
                             const char *cookie_val, edge_principal_t *out);

/**
 * Extract edge_session= value from Cookie header line value (may be multi).
 * @return 0 found, -1 not found.
 */
int edge_auth_cookie_extract(const char *cookie_hdr, char *out, size_t out_sz);

/**
 * Constant-time password compare (pads to max).
 * @return 1 match, 0 mismatch.
 */
int edge_auth_password_ok(const edge_auth_ctx_t *ctx, const char *password,
                          size_t password_len);

/**
 * Pull "password" string from a small JSON object body.
 * @return 0 ok, -1 parse error.
 */
int edge_auth_parse_login_password(const char *body, size_t body_len, char *out,
                                   size_t out_sz);

/** Map HTTP method + path to resource (0 = not protected / unknown). */
edge_auth_resource_t edge_auth_classify(const char *method, const char *path,
                                        int state_is_list);

/** True when mode requires principal for protected routes. */
int edge_auth_mode_enforced(edge_auth_mode_t mode);

/**
 * Parse comma-separated role names into role bits.
 * Empty/NULL → EDGE_ROLE_EMPLOYEE (proxy default).
 */
uint32_t edge_auth_roles_from_csv(const char *csv);

/**
 * Canonical string for proxy HMAC (NUL-terminated in out):
 *   "v1\\n" + ts + "\\n" + user + "\\n" + roles
 * @return bytes written excl NUL, or -1.
 */
int edge_auth_proxy_canonical(const char *user, const char *roles_csv,
                              const char *ts, char *out, size_t out_sz);

/**
 * Sign proxy headers (base64url HMAC-SHA256 of canonical).
 * @p roles_csv may be NULL → "employee".
 * @return 0 ok, -1 error.
 */
int edge_auth_proxy_sign(const edge_auth_ctx_t *ctx, const char *user,
                         const char *roles_csv, int64_t ts, char *sig_out,
                         size_t sig_out_sz);

/**
 * Verify X-User / X-Roles / X-Auth-Timestamp / X-Auth-Signature.
 * roles_hdr may be NULL/empty → employee.
 * Checks HMAC and |now - ts| <= proxy_max_skew_s.
 * @return 0 ok (principal filled), -1 invalid.
 */
int edge_auth_proxy_verify(const edge_auth_ctx_t *ctx, const char *user,
                           const char *roles_hdr, const char *ts_hdr,
                           const char *sig_hdr, edge_principal_t *out);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_AUTH_H */
