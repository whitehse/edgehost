/**
 * @file auth_rbac.c
 * @brief Pure RBAC + lab sessions (P1.7c) + proxy header HMAC (P1.7d).
 */

#include "edge_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- SHA-256 + HMAC (compact, public-domain style) ------------------------ */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  data[64];
    size_t   datalen;
} sha256_ctx_t;

static uint32_t rotr(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[])
{
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
        0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
        0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
        0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
        0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
        0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
        0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
        0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    uint32_t m[64];
    uint32_t a, b, c, d, e, f, g, h, t1, t2;
    int i, j;

    for (i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    }
    for (; i < 64; ++i) {
        uint32_t s0 = rotr(m[i - 15], 7) ^ rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
        uint32_t s1 = rotr(m[i - 2], 17) ^ rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];
    for (i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        t1 = h + S1 + ch + k[i] + m[i];
        {
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            t2 = S0 + maj;
        }
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t data[], size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32])
{
    size_t i = ctx->datalen;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) {
            ctx->data[i++] = 0x00;
        }
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) {
            ctx->data[i++] = 0x00;
        }
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);
    for (i = 0; i < 4; ++i) {
        hash[i] = (uint8_t)((ctx->state[0] >> (24 - i * 8)) & 0xff);
        hash[i + 4] = (uint8_t)((ctx->state[1] >> (24 - i * 8)) & 0xff);
        hash[i + 8] = (uint8_t)((ctx->state[2] >> (24 - i * 8)) & 0xff);
        hash[i + 12] = (uint8_t)((ctx->state[3] >> (24 - i * 8)) & 0xff);
        hash[i + 16] = (uint8_t)((ctx->state[4] >> (24 - i * 8)) & 0xff);
        hash[i + 20] = (uint8_t)((ctx->state[5] >> (24 - i * 8)) & 0xff);
        hash[i + 24] = (uint8_t)((ctx->state[6] >> (24 - i * 8)) & 0xff);
        hash[i + 28] = (uint8_t)((ctx->state[7] >> (24 - i * 8)) & 0xff);
    }
}

static void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg,
                        size_t msg_len, uint8_t out[32])
{
    uint8_t k[64];
    uint8_t o_key[64];
    uint8_t i_key[64];
    uint8_t inner[32];
    sha256_ctx_t ctx;
    size_t i;

    memset(k, 0, sizeof(k));
    if (key_len > 64) {
        sha256_init(&ctx);
        sha256_update(&ctx, key, key_len);
        sha256_final(&ctx, k);
    } else {
        memcpy(k, key, key_len);
    }
    for (i = 0; i < 64; i++) {
        o_key[i] = (uint8_t)(k[i] ^ 0x5c);
        i_key[i] = (uint8_t)(k[i] ^ 0x36);
    }
    sha256_init(&ctx);
    sha256_update(&ctx, i_key, 64);
    sha256_update(&ctx, msg, msg_len);
    sha256_final(&ctx, inner);
    sha256_init(&ctx);
    sha256_update(&ctx, o_key, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
}

/* --- base64url ------------------------------------------------------------ */

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_sz,
                      int url)
{
    size_t i = 0, o = 0;
    size_t need = ((in_len + 2) / 3) * 4 + 1;

    if (!in || !out || out_sz < need) {
        return -1;
    }
    while (i + 2 < in_len) {
        uint32_t n = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) |
                     (uint32_t)in[i + 2];
        out[o++] = b64[(n >> 18) & 63];
        out[o++] = b64[(n >> 12) & 63];
        out[o++] = b64[(n >> 6) & 63];
        out[o++] = b64[n & 63];
        i += 3;
    }
    if (i < in_len) {
        uint32_t n = (uint32_t)in[i] << 16;
        out[o++] = b64[(n >> 18) & 63];
        if (i + 1 < in_len) {
            n |= (uint32_t)in[i + 1] << 8;
            out[o++] = b64[(n >> 12) & 63];
            out[o++] = b64[(n >> 6) & 63];
            out[o++] = '=';
        } else {
            out[o++] = b64[(n >> 12) & 63];
            out[o++] = '=';
            out[o++] = '=';
        }
    }
    out[o] = '\0';
    if (url) {
        for (i = 0; out[i]; i++) {
            if (out[i] == '+') {
                out[i] = '-';
            } else if (out[i] == '/') {
                out[i] = '_';
            }
        }
        /* strip padding for compact cookie */
        while (o > 0 && out[o - 1] == '=') {
            out[--o] = '\0';
        }
    }
    return 0;
}

static int b64_decode_char(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+' || c == '-') {
        return 62;
    }
    if (c == '/' || c == '_') {
        return 63;
    }
    return -1;
}

static int b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_sz,
                      size_t *out_len)
{
    size_t i = 0, o = 0;
    int val = 0, valb = -8;
    char pad[8];
    size_t pad_len = in_len;
    const char *src = in;

    /* restore padding for standard decode */
    if (in_len % 4 != 0) {
        size_t need = 4 - (in_len % 4);
        if (in_len + need + 1 > sizeof(pad) + in_len) {
            /* use stack for small cookies only */
        }
        if (in_len + need >= 512) {
            return -1;
        }
    }
    {
        char tmp[512];
        if (in_len >= sizeof(tmp) - 4) {
            return -1;
        }
        memcpy(tmp, in, in_len);
        pad_len = in_len;
        while (pad_len % 4 != 0) {
            tmp[pad_len++] = '=';
        }
        tmp[pad_len] = '\0';
        src = tmp;
        in_len = pad_len;
        (void)pad;

        for (i = 0; i < in_len; i++) {
            int c;
            if (src[i] == '=') {
                break;
            }
            c = b64_decode_char(src[i]);
            if (c < 0) {
                return -1;
            }
            val = (val << 6) + c;
            valb += 6;
            if (valb >= 0) {
                if (o >= out_sz) {
                    return -1;
                }
                out[o++] = (uint8_t)((val >> valb) & 0xFF);
                valb -= 8;
            }
        }
    }
    if (out_len) {
        *out_len = o;
    }
    return 0;
}

static int ct_eq(const uint8_t *a, const uint8_t *b, size_t n)
{
    size_t i;
    uint8_t d = 0;
    for (i = 0; i < n; i++) {
        d |= (uint8_t)(a[i] ^ b[i]);
    }
    return d == 0;
}

/* --- public API ----------------------------------------------------------- */

void edge_auth_ctx_init(edge_auth_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->mode = EDGE_AUTH_MODE_OPEN;
    ctx->session_ttl_s = 28800;
    ctx->proxy_max_skew_s = 300;
}

void edge_principal_clear(edge_principal_t *p)
{
    if (p) {
        memset(p, 0, sizeof(*p));
    }
}

int edge_auth_role_has(uint32_t roles, edge_role_t role)
{
    return (roles & (uint32_t)role) != 0;
}

const char *edge_auth_mode_name(edge_auth_mode_t m)
{
    switch (m) {
    case EDGE_AUTH_MODE_OPEN:          return "open";
    case EDGE_AUTH_MODE_LAB_PASSWORD:  return "lab_password";
    case EDGE_AUTH_MODE_PROXY_HEADERS: return "proxy_headers";
    default:                           return "unknown";
    }
}

const char *edge_auth_resource_name(edge_auth_resource_t r)
{
    switch (r) {
    case EDGE_RES_STATE_GET:    return "STATE_GET";
    case EDGE_RES_STATE_PUT:    return "STATE_PUT";
    case EDGE_RES_STATE_DELETE: return "STATE_DELETE";
    case EDGE_RES_STATE_LIST:   return "STATE_LIST";
    case EDGE_RES_WS_STREAM:    return "WS_STREAM";
    case EDGE_RES_PACKAGES:     return "PACKAGES";
    case EDGE_RES_OPENAI:       return "OPENAI";
    case EDGE_RES_E7_GET:       return "E7_GET";
    case EDGE_RES_E7_ADMIN:     return "E7_ADMIN";
    case EDGE_RES_EXPLAIN:      return "EXPLAIN";
    default:                    return "NONE";
    }
}

uint32_t edge_auth_role_parse(const char *name)
{
    if (!name) {
        return 0;
    }
    if (strcmp(name, "employee") == 0) {
        return EDGE_ROLE_EMPLOYEE;
    }
    if (strcmp(name, "employee_admin") == 0) {
        return EDGE_ROLE_EMPLOYEE_ADMIN;
    }
    if (strcmp(name, "cpe") == 0) {
        return EDGE_ROLE_CPE;
    }
    if (strcmp(name, "customer") == 0) {
        return EDGE_ROLE_CUSTOMER;
    }
    if (strcmp(name, "service_openai") == 0) {
        return EDGE_ROLE_SERVICE_OPENAI;
    }
    if (strcmp(name, "ingest") == 0) {
        return EDGE_ROLE_INGEST;
    }
    return 0;
}

edge_auth_decision_t edge_auth_rbac_check(const edge_principal_t *p,
                                          edge_auth_resource_t res,
                                          const char *ns, const char *key)
{
    (void)ns;
    (void)key;
    if (!p || !p->authenticated) {
        return EDGE_AUTH_DENY;
    }
    if (edge_auth_role_has(p->roles, EDGE_ROLE_EMPLOYEE_ADMIN)) {
        switch (res) {
        case EDGE_RES_STATE_GET:
        case EDGE_RES_STATE_PUT:
        case EDGE_RES_STATE_DELETE:
        case EDGE_RES_STATE_LIST:
        case EDGE_RES_WS_STREAM:
        case EDGE_RES_PACKAGES:
        case EDGE_RES_OPENAI:
        case EDGE_RES_E7_GET:
        case EDGE_RES_E7_ADMIN:
        case EDGE_RES_EXPLAIN:
        case EDGE_RES_TELEMETRY:
            return EDGE_AUTH_ALLOW;
        default:
            return EDGE_AUTH_DENY;
        }
    }
    if (edge_auth_role_has(p->roles, EDGE_ROLE_EMPLOYEE)) {
        switch (res) {
        case EDGE_RES_STATE_GET:
        case EDGE_RES_STATE_PUT:
        case EDGE_RES_STATE_DELETE:
        case EDGE_RES_STATE_LIST:
        case EDGE_RES_WS_STREAM:
        case EDGE_RES_PACKAGES:
        case EDGE_RES_OPENAI:
        case EDGE_RES_E7_GET:
        case EDGE_RES_EXPLAIN:
        case EDGE_RES_TELEMETRY:
            return EDGE_AUTH_ALLOW;
        case EDGE_RES_E7_ADMIN:
            return EDGE_AUTH_DENY;
        default:
            return EDGE_AUTH_DENY;
        }
    }
    if (edge_auth_role_has(p->roles, EDGE_ROLE_SERVICE_OPENAI)) {
        if (res == EDGE_RES_OPENAI) {
            return EDGE_AUTH_ALLOW;
        }
        return EDGE_AUTH_DENY;
    }
    if (edge_auth_role_has(p->roles, EDGE_ROLE_INGEST)) {
        if (res == EDGE_RES_STATE_PUT || res == EDGE_RES_TELEMETRY) {
            return EDGE_AUTH_ALLOW;
        }
        return EDGE_AUTH_DENY;
    }
    /* customer / cpe: deny for v1 global state until ns enabled with rules */
    return EDGE_AUTH_DENY;
}

int64_t edge_auth_now_sec(const edge_auth_ctx_t *ctx)
{
    if (ctx && ctx->now_sec_override > 0) {
        return ctx->now_sec_override;
    }
    return (int64_t)time(NULL);
}

static int roles_to_json_array(uint32_t roles, char *out, size_t out_sz)
{
    char tmp[128];
    size_t n = 0;
    int first = 1;

    tmp[0] = '[';
    n = 1;
#define ADD(bit, name)                                                         \
    do {                                                                       \
        if (roles & (bit)) {                                                   \
            int w = snprintf(tmp + n, sizeof(tmp) - n, "%s\"%s\"",             \
                             first ? "" : ",", name);                          \
            if (w < 0 || (size_t)w >= sizeof(tmp) - n) {                       \
                return -1;                                                     \
            }                                                                  \
            n += (size_t)w;                                                    \
            first = 0;                                                         \
        }                                                                      \
    } while (0)
    ADD(EDGE_ROLE_EMPLOYEE, "employee");
    ADD(EDGE_ROLE_EMPLOYEE_ADMIN, "employee_admin");
    ADD(EDGE_ROLE_CPE, "cpe");
    ADD(EDGE_ROLE_CUSTOMER, "customer");
    ADD(EDGE_ROLE_SERVICE_OPENAI, "service_openai");
    ADD(EDGE_ROLE_INGEST, "ingest");
#undef ADD
    if (n + 2 > sizeof(tmp)) {
        return -1;
    }
    tmp[n++] = ']';
    tmp[n] = '\0';
    if (n + 1 > out_sz) {
        return -1;
    }
    memcpy(out, tmp, n + 1);
    return 0;
}

static uint32_t parse_roles_array(const char *json)
{
    uint32_t roles = 0;
    const char *p = strstr(json, "\"roles\"");
    if (!p) {
        return 0;
    }
    p = strchr(p, '[');
    if (!p) {
        return 0;
    }
    {
        const char *end = strchr(p, ']');
        char buf[128];
        size_t len;
        if (!end || (size_t)(end - p) >= sizeof(buf)) {
            return 0;
        }
        len = (size_t)(end - p + 1);
        memcpy(buf, p, len);
        buf[len] = '\0';
        if (strstr(buf, "\"employee_admin\"")) {
            roles |= EDGE_ROLE_EMPLOYEE_ADMIN;
        }
        if (strstr(buf, "\"employee\"")) {
            roles |= EDGE_ROLE_EMPLOYEE;
        }
        if (strstr(buf, "\"cpe\"")) {
            roles |= EDGE_ROLE_CPE;
        }
        if (strstr(buf, "\"customer\"")) {
            roles |= EDGE_ROLE_CUSTOMER;
        }
        if (strstr(buf, "\"service_openai\"")) {
            roles |= EDGE_ROLE_SERVICE_OPENAI;
        }
        if (strstr(buf, "\"ingest\"")) {
            roles |= EDGE_ROLE_INGEST;
        }
    }
    return roles;
}

static int parse_json_string_field(const char *json, const char *field, char *out,
                                   size_t out_sz)
{
    char key[80];
    const char *p;
    size_t i = 0;

    snprintf(key, sizeof(key), "\"%s\"", field);
    p = strstr(json, key);
    if (!p) {
        return -1;
    }
    p = strchr(p + strlen(key), '"');
    if (!p) {
        return -1;
    }
    p++;
    while (*p && *p != '"' && i + 1 < out_sz) {
        if (*p == '\\' && p[1]) {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (*p == '"') ? 0 : -1;
}

static int parse_json_int64_field(const char *json, const char *field,
                                  int64_t *out)
{
    char key[80];
    const char *p;
    char *end = NULL;
    long long v;

    snprintf(key, sizeof(key), "\"%s\"", field);
    p = strstr(json, key);
    if (!p) {
        return -1;
    }
    p = strchr(p + strlen(key), ':');
    if (!p) {
        return -1;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    v = strtoll(p, &end, 10);
    if (end == p) {
        return -1;
    }
    *out = (int64_t)v;
    return 0;
}

int edge_auth_session_issue(const edge_auth_ctx_t *ctx, const char *sub,
                            uint32_t roles, char *cookie_val,
                            size_t cookie_val_sz, edge_principal_t *out_p)
{
    char payload[256];
    char roles_json[128];
    char pay_b64[400];
    char sig_b64[64];
    uint8_t mac[32];
    int64_t exp;
    int n;

    if (!ctx || !cookie_val || cookie_val_sz < 16 || ctx->hmac_key_len == 0) {
        return -1;
    }
    if (!sub || !sub[0]) {
        sub = "lab";
    }
    if (roles_to_json_array(roles, roles_json, sizeof(roles_json)) != 0) {
        return -1;
    }
    exp = edge_auth_now_sec(ctx) +
          (int64_t)(ctx->session_ttl_s ? ctx->session_ttl_s : 28800);
    n = snprintf(payload, sizeof(payload),
                 "{\"sub\":\"%s\",\"roles\":%s,\"exp\":%lld}", sub, roles_json,
                 (long long)exp);
    if (n < 0 || (size_t)n >= sizeof(payload)) {
        return -1;
    }
    if (b64_encode((const uint8_t *)payload, (size_t)n, pay_b64, sizeof(pay_b64),
                   1) != 0) {
        return -1;
    }
    hmac_sha256(ctx->hmac_key, ctx->hmac_key_len, (const uint8_t *)payload,
                (size_t)n, mac);
    if (b64_encode(mac, 32, sig_b64, sizeof(sig_b64), 1) != 0) {
        return -1;
    }
    n = snprintf(cookie_val, cookie_val_sz, "%s.%s", pay_b64, sig_b64);
    if (n < 0 || (size_t)n >= cookie_val_sz) {
        return -1;
    }
    if (out_p) {
        edge_principal_clear(out_p);
        out_p->authenticated = 1;
        snprintf(out_p->sub, sizeof(out_p->sub), "%s", sub);
        out_p->roles = roles;
        out_p->exp = exp;
    }
    return 0;
}

int edge_auth_session_verify(const edge_auth_ctx_t *ctx, const char *cookie_val,
                             edge_principal_t *out)
{
    const char *dot;
    char pay_b64[400];
    char sig_b64[64];
    uint8_t payload[320];
    size_t pay_len = 0;
    uint8_t sig[32];
    size_t sig_len = 0;
    uint8_t mac[32];
    char json[320];
    int64_t exp = 0;
    char sub[EDGE_AUTH_SUB_MAX];

    if (out) {
        edge_principal_clear(out);
    }
    if (!ctx || !cookie_val || !out || ctx->hmac_key_len == 0) {
        return -1;
    }
    dot = strchr(cookie_val, '.');
    if (!dot || dot == cookie_val || !dot[1]) {
        return -1;
    }
    if ((size_t)(dot - cookie_val) >= sizeof(pay_b64)) {
        return -1;
    }
    memcpy(pay_b64, cookie_val, (size_t)(dot - cookie_val));
    pay_b64[dot - cookie_val] = '\0';
    if (strlen(dot + 1) >= sizeof(sig_b64)) {
        return -1;
    }
    snprintf(sig_b64, sizeof(sig_b64), "%s", dot + 1);

    if (b64_decode(pay_b64, strlen(pay_b64), payload, sizeof(payload),
                   &pay_len) != 0 ||
        pay_len == 0 || pay_len >= sizeof(json)) {
        return -1;
    }
    memcpy(json, payload, pay_len);
    json[pay_len] = '\0';

    if (b64_decode(sig_b64, strlen(sig_b64), sig, sizeof(sig), &sig_len) != 0 ||
        sig_len != 32) {
        return -1;
    }
    hmac_sha256(ctx->hmac_key, ctx->hmac_key_len, payload, pay_len, mac);
    if (!ct_eq(mac, sig, 32)) {
        return -1;
    }
    if (parse_json_string_field(json, "sub", sub, sizeof(sub)) != 0) {
        return -1;
    }
    if (parse_json_int64_field(json, "exp", &exp) != 0) {
        return -1;
    }
    if (exp > 0 && edge_auth_now_sec(ctx) >= exp) {
        return -1;
    }
    out->authenticated = 1;
    snprintf(out->sub, sizeof(out->sub), "%s", sub);
    out->roles = parse_roles_array(json);
    out->exp = exp;
    return 0;
}

int edge_auth_cookie_extract(const char *cookie_hdr, char *out, size_t out_sz)
{
    const char *p;
    const char *name = EDGE_AUTH_COOKIE_NAME;
    size_t nlen;

    if (!cookie_hdr || !out || out_sz == 0) {
        return -1;
    }
    nlen = strlen(name);
    p = cookie_hdr;
    while (*p) {
        while (*p == ' ' || *p == ';') {
            p++;
        }
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            size_t i = 0;
            p += nlen + 1;
            while (*p && *p != ';' && i + 1 < out_sz) {
                out[i++] = *p++;
            }
            out[i] = '\0';
            return i > 0 ? 0 : -1;
        }
        while (*p && *p != ';') {
            p++;
        }
    }
    return -1;
}

int edge_auth_password_ok(const edge_auth_ctx_t *ctx, const char *password,
                          size_t password_len)
{
    size_t expect_len;
    size_t i;
    uint8_t d = 0;
    const char *expect;

    if (!ctx || !password) {
        return 0;
    }
    expect = ctx->lab_password;
    expect_len = strlen(expect);
    if (expect_len == 0) {
        return 0;
    }
    /* constant-time over max of both lengths (bounded) */
    {
        size_t max = expect_len > password_len ? expect_len : password_len;
        if (max > EDGE_AUTH_PASSWORD_MAX) {
            max = EDGE_AUTH_PASSWORD_MAX;
        }
        for (i = 0; i < max; i++) {
            unsigned char a = i < password_len ? (unsigned char)password[i] : 0;
            unsigned char b = i < expect_len ? (unsigned char)expect[i] : 0;
            d |= (uint8_t)(a ^ b);
        }
        d |= (uint8_t)(expect_len ^ password_len);
    }
    return d == 0;
}

int edge_auth_parse_login_password(const char *body, size_t body_len, char *out,
                                   size_t out_sz)
{
    char tmp[512];
    const char *p;
    size_t i = 0;

    if (!body || !out || out_sz == 0 || body_len == 0 || body_len >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, body, body_len);
    tmp[body_len] = '\0';
    p = strstr(tmp, "\"password\"");
    if (!p) {
        return -1;
    }
    /* Skip past key "password" (10 chars) then find value opening quote. */
    p = strchr(p + 10, '"');
    if (!p) {
        return -1;
    }
    p++; /* first char of password value */
    while (*p && *p != '"' && i + 1 < out_sz) {
        if (*p == '\\' && p[1]) {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (*p == '"') ? 0 : -1;
}

edge_auth_resource_t edge_auth_classify(const char *method, const char *path,
                                        int state_is_list)
{
    if (!method || !path) {
        return EDGE_RES_NONE;
    }
    if (strncmp(path, "/api/v1/stream", 14) == 0 &&
        (path[14] == '\0' || path[14] == '?')) {
        if (strcmp(method, "GET") == 0) {
            return EDGE_RES_WS_STREAM;
        }
        return EDGE_RES_NONE;
    }
    if (strncmp(path, "/api/v1/state/", 14) == 0) {
        if (strcmp(method, "GET") == 0) {
            return state_is_list ? EDGE_RES_STATE_LIST : EDGE_RES_STATE_GET;
        }
        if (strcmp(method, "PUT") == 0) {
            return EDGE_RES_STATE_PUT;
        }
        if (strcmp(method, "DELETE") == 0) {
            return EDGE_RES_STATE_DELETE;
        }
    }
    if (strncmp(path, "/packages", 9) == 0 &&
        (path[9] == '\0' || path[9] == '/' || path[9] == '?')) {
        if (strcmp(method, "GET") == 0) {
            return EDGE_RES_PACKAGES;
        }
    }
    if (strncmp(path, "/v1/", 4) == 0 || strcmp(path, "/v1") == 0) {
        return EDGE_RES_OPENAI;
    }
    /* Lab diagnostics: /api/v1/debug/... (employee+; same gate as E7 GET) */
    if (strncmp(path, "/api/v1/debug", 13) == 0 &&
        (path[13] == '\0' || path[13] == '/' || path[13] == '?')) {
        if (strcmp(method, "GET") == 0 || strcmp(method, "POST") == 0) {
            return EDGE_RES_E7_GET;
        }
    }
    /* CPE telemetry proxy → ClickHouse (ingest + employee) */
    if (strncmp(path, "/api/v1/telemetry", 17) == 0 &&
        (path[17] == '\0' || path[17] == '/' || path[17] == '?')) {
        if (strcmp(method, "GET") == 0 || strcmp(method, "POST") == 0) {
            return EDGE_RES_TELEMETRY;
        }
    }
    /* Certificate Authority (admin vs sign) */
    if (strncmp(path, "/api/v1/ca", 10) == 0 &&
        (path[10] == '\0' || path[10] == '/' || path[10] == '?')) {
        if (strcmp(method, "POST") == 0 || strstr(path, "/revoke") != NULL) {
            if (strstr(path, "/sign") != NULL) {
                return EDGE_RES_E7_GET; /* CPE CSR sign */
            }
            return EDGE_RES_E7_ADMIN;
        }
        if (strcmp(method, "GET") == 0) {
            return EDGE_RES_E7_GET;
        }
    }
    /* E7 Call Home REST: /api/v1/e7 and /api/v1/e7/... */
    if (strncmp(path, "/api/v1/e7", 10) == 0 &&
        (path[10] == '\0' || path[10] == '/' || path[10] == '?')) {
        if (strcmp(method, "GET") == 0) {
            return EDGE_RES_E7_GET;
        }
        if (strcmp(method, "PUT") == 0 || strcmp(method, "DELETE") == 0 ||
            strcmp(method, "POST") == 0) {
            return EDGE_RES_E7_ADMIN;
        }
    }
    /* Fiber explain: /api/v1/explain and /api/v1/explain/... */
    if (strncmp(path, "/api/v1/explain", 15) == 0 &&
        (path[15] == '\0' || path[15] == '/' || path[15] == '?')) {
        if (strcmp(method, "GET") == 0 || strcmp(method, "POST") == 0) {
            return EDGE_RES_EXPLAIN;
        }
    }
    return EDGE_RES_NONE;
}

int edge_auth_mode_enforced(edge_auth_mode_t mode)
{
    return mode == EDGE_AUTH_MODE_LAB_PASSWORD ||
           mode == EDGE_AUTH_MODE_PROXY_HEADERS;
}

uint32_t edge_auth_roles_from_csv(const char *csv)
{
    uint32_t roles = 0;
    const char *p;
    char tok[64];
    size_t ti;

    if (!csv || !csv[0]) {
        return EDGE_ROLE_EMPLOYEE;
    }
    p = csv;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        ti = 0;
        while (*p && *p != ',' && *p != ' ' && *p != '\t' &&
               ti + 1 < sizeof(tok)) {
            tok[ti++] = *p++;
        }
        tok[ti] = '\0';
        if (ti > 0) {
            uint32_t bit = edge_auth_role_parse(tok);
            if (bit) {
                roles |= bit;
            }
        }
        while (*p && *p != ',') {
            p++;
        }
    }
    return roles ? roles : EDGE_ROLE_EMPLOYEE;
}

int edge_auth_proxy_canonical(const char *user, const char *roles_csv,
                              const char *ts, char *out, size_t out_sz)
{
    int n;

    if (!user || !user[0] || !ts || !ts[0] || !out || out_sz < 8) {
        return -1;
    }
    if (!roles_csv || !roles_csv[0]) {
        roles_csv = "employee";
    }
    n = snprintf(out, out_sz, "v1\n%s\n%s\n%s", ts, user, roles_csv);
    if (n < 0 || (size_t)n >= out_sz) {
        return -1;
    }
    return n;
}

int edge_auth_proxy_sign(const edge_auth_ctx_t *ctx, const char *user,
                         const char *roles_csv, int64_t ts, char *sig_out,
                         size_t sig_out_sz)
{
    char canon[256];
    char tsbuf[32];
    uint8_t mac[32];
    int n;

    if (!ctx || ctx->proxy_hmac_key_len == 0 || !sig_out) {
        return -1;
    }
    snprintf(tsbuf, sizeof(tsbuf), "%lld", (long long)ts);
    n = edge_auth_proxy_canonical(user, roles_csv, tsbuf, canon, sizeof(canon));
    if (n < 0) {
        return -1;
    }
    hmac_sha256(ctx->proxy_hmac_key, ctx->proxy_hmac_key_len,
                (const uint8_t *)canon, (size_t)n, mac);
    return b64_encode(mac, 32, sig_out, sig_out_sz, 1);
}

int edge_auth_proxy_verify(const edge_auth_ctx_t *ctx, const char *user,
                           const char *roles_hdr, const char *ts_hdr,
                           const char *sig_hdr, edge_principal_t *out)
{
    char canon[256];
    uint8_t mac[32];
    uint8_t sig[32];
    size_t sig_len = 0;
    int n;
    int64_t ts = 0;
    int64_t now;
    int64_t skew;
    uint32_t max_skew;
    char *end = NULL;
    const char *roles_use;

    if (out) {
        edge_principal_clear(out);
    }
    if (!ctx || !user || !user[0] || !ts_hdr || !sig_hdr || !out) {
        return -1;
    }
    if (ctx->proxy_hmac_key_len == 0) {
        return -1;
    }
    if (strlen(user) >= EDGE_AUTH_SUB_MAX) {
        return -1;
    }
    /* reject user with control chars / newlines (canonical injection) */
    {
        const char *u = user;
        while (*u) {
            if ((unsigned char)*u < 0x20 || *u == '\n' || *u == '\r') {
                return -1;
            }
            u++;
        }
    }
    roles_use = (roles_hdr && roles_hdr[0]) ? roles_hdr : "employee";
    {
        const char *r = roles_use;
        while (*r) {
            if ((unsigned char)*r < 0x20 || *r == '\n' || *r == '\r') {
                return -1;
            }
            r++;
        }
    }

    ts = (int64_t)strtoll(ts_hdr, &end, 10);
    if (end == ts_hdr || *end != '\0' || ts <= 0) {
        return -1;
    }
    now = edge_auth_now_sec(ctx);
    max_skew = ctx->proxy_max_skew_s ? ctx->proxy_max_skew_s : 300;
    skew = now > ts ? now - ts : ts - now;
    if (skew > (int64_t)max_skew) {
        return -1;
    }

    n = edge_auth_proxy_canonical(user, roles_use, ts_hdr, canon, sizeof(canon));
    if (n < 0) {
        return -1;
    }
    hmac_sha256(ctx->proxy_hmac_key, ctx->proxy_hmac_key_len,
                (const uint8_t *)canon, (size_t)n, mac);
    if (b64_decode(sig_hdr, strlen(sig_hdr), sig, sizeof(sig), &sig_len) != 0 ||
        sig_len != 32) {
        return -1;
    }
    if (!ct_eq(mac, sig, 32)) {
        return -1;
    }

    out->authenticated = 1;
    snprintf(out->sub, sizeof(out->sub), "%s", user);
    out->roles = edge_auth_roles_from_csv(roles_use);
    out->exp = ts + (int64_t)max_skew;
    return 0;
}
