/**
 * @file ws_stream.c
 * @brief RFC 6455 accept key, STATE_CHANGED JSON, WS fan-out hub (P1.7b).
 */

#include "edge_ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- compact SHA-1 (public domain style) ---------------------------------- */

typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
} sha1_ctx_t;

static uint32_t sha1_rol(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static void sha1_transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    uint32_t a, b, c, d, e, f, k, t;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (i = 16; i < 80; i++) {
        w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        t = sha1_rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = sha1_rol(b, 30);
        b = a;
        a = t;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void sha1_init(sha1_ctx_t *ctx)
{
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xEFCDAB89u;
    ctx->state[2] = 0x98BADCFEu;
    ctx->state[3] = 0x10325476u;
    ctx->state[4] = 0xC3D2E1F0u;
    ctx->count = 0;
}

static void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t i = 0;
    size_t idx = (size_t)(ctx->count & 63u);

    ctx->count += len;
    while (i < len) {
        ctx->buffer[idx++] = data[i++];
        if (idx == 64) {
            sha1_transform(ctx->state, ctx->buffer);
            idx = 0;
        }
    }
}

static void sha1_final(sha1_ctx_t *ctx, uint8_t dig[20])
{
    uint8_t pad[64];
    uint64_t bits = ctx->count * 8;
    size_t idx = (size_t)(ctx->count & 63u);
    size_t pad_len;
    int i;

    pad[0] = 0x80;
    if (idx < 56) {
        pad_len = 56 - idx;
    } else {
        pad_len = 120 - idx;
    }
    memset(pad + 1, 0, pad_len - 1);
    sha1_update(ctx, pad, pad_len);
    {
        uint8_t lenb[8];
        for (i = 0; i < 8; i++) {
            lenb[7 - i] = (uint8_t)(bits >> (i * 8));
        }
        sha1_update(ctx, lenb, 8);
    }
    for (i = 0; i < 5; i++) {
        dig[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        dig[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        dig[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        dig[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

/* --- base64 --------------------------------------------------------------- */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_sz)
{
    size_t i = 0, o = 0;
    size_t need = ((in_len + 2) / 3) * 4 + 1;

    if (!in || !out || out_sz < need) {
        return -1;
    }
    while (i + 2 < in_len) {
        uint32_t n = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) |
                     (uint32_t)in[i + 2];
        out[o++] = b64_table[(n >> 18) & 63];
        out[o++] = b64_table[(n >> 12) & 63];
        out[o++] = b64_table[(n >> 6) & 63];
        out[o++] = b64_table[n & 63];
        i += 3;
    }
    if (i < in_len) {
        uint32_t n = (uint32_t)in[i] << 16;
        out[o++] = b64_table[(n >> 18) & 63];
        if (i + 1 < in_len) {
            n |= (uint32_t)in[i + 1] << 8;
            out[o++] = b64_table[(n >> 12) & 63];
            out[o++] = b64_table[(n >> 6) & 63];
            out[o++] = '=';
        } else {
            out[o++] = b64_table[(n >> 12) & 63];
            out[o++] = '=';
            out[o++] = '=';
        }
    }
    out[o] = '\0';
    return 0;
}

/* GUID from RFC 6455 */
static const char ws_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

int edge_ws_accept_key(const char *sec_ws_key, char *out, size_t out_sz)
{
    sha1_ctx_t ctx;
    uint8_t dig[20];
    char tmp[EDGE_WS_KEY_MAX + 64];
    size_t klen;

    if (!sec_ws_key || !out || out_sz < 29) {
        return -1;
    }
    /* trim */
    while (*sec_ws_key == ' ' || *sec_ws_key == '\t') {
        sec_ws_key++;
    }
    klen = strlen(sec_ws_key);
    while (klen > 0 &&
           (sec_ws_key[klen - 1] == ' ' || sec_ws_key[klen - 1] == '\t' ||
            sec_ws_key[klen - 1] == '\r' || sec_ws_key[klen - 1] == '\n')) {
        klen--;
    }
    if (klen == 0 || klen >= EDGE_WS_KEY_MAX) {
        return -1;
    }
    memcpy(tmp, sec_ws_key, klen);
    memcpy(tmp + klen, ws_guid, sizeof(ws_guid) - 1);
    tmp[klen + sizeof(ws_guid) - 1] = '\0';

    sha1_init(&ctx);
    sha1_update(&ctx, (const uint8_t *)tmp, klen + sizeof(ws_guid) - 1);
    sha1_final(&ctx, dig);
    return b64_encode(dig, 20, out, out_sz);
}

int edge_ws_build_101(const char *sec_ws_key, char *out, size_t out_cap,
                      size_t *out_len)
{
    char accept[EDGE_WS_ACCEPT_MAX];
    int n;

    if (!out || !out_len || edge_ws_accept_key(sec_ws_key, accept,
                                               sizeof(accept)) != 0) {
        return -1;
    }
    n = snprintf(out, out_cap,
                 "HTTP/1.1 101 Switching Protocols\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: %s\r\n"
                 "\r\n",
                 accept);
    if (n < 0 || (size_t)n >= out_cap) {
        return -1;
    }
    *out_len = (size_t)n;
    return 0;
}

int edge_ws_path_is_stream(const char *path)
{
    if (!path) {
        return 0;
    }
    if (strcmp(path, "/api/v1/stream") == 0) {
        return 1;
    }
    if (strncmp(path, "/api/v1/stream?", 15) == 0) {
        return 1;
    }
    return 0;
}

int edge_ws_format_state_changed(char *out, size_t out_cap, const char *ns,
                                 const char *key, const char *op,
                                 const char *value, size_t value_len,
                                 const char *request_id)
{
    int n;

    if (!out || out_cap < 32 || !ns || !key || !op) {
        return -1;
    }
    if (!request_id) {
        request_id = "";
    }
    if (value && value_len > 0) {
        /* value is already JSON; embed raw */
        n = snprintf(out, out_cap,
                     "{\"type\":\"STATE_CHANGED\",\"ns\":\"%s\",\"key\":\"%s\","
                     "\"op\":\"%s\",\"value\":",
                     ns, key, op);
        if (n < 0 || (size_t)n >= out_cap) {
            return -1;
        }
        if ((size_t)n + value_len + 40 >= out_cap) {
            return -1;
        }
        memcpy(out + n, value, value_len);
        n += (int)value_len;
        {
            int m = snprintf(out + n, out_cap - (size_t)n,
                             ",\"request_id\":\"%s\"}", request_id);
            if (m < 0 || (size_t)n + (size_t)m >= out_cap) {
                return -1;
            }
            n += m;
        }
    } else {
        n = snprintf(out, out_cap,
                     "{\"type\":\"STATE_CHANGED\",\"ns\":\"%s\",\"key\":\"%s\","
                     "\"op\":\"%s\",\"value\":null,\"request_id\":\"%s\"}",
                     ns, key, op, request_id);
        if (n < 0 || (size_t)n >= out_cap) {
            return -1;
        }
    }
    return n;
}

/* --- hub ------------------------------------------------------------------ */

typedef struct {
    int    active;
    char  *q[EDGE_WS_PENDING_MAX];
    size_t qlen[EDGE_WS_PENDING_MAX];
    int    head;
    int    tail;
    int    n;
} edge_ws_sub_t;

struct edge_ws_hub {
    edge_ws_sub_t *subs;
    size_t         max_subs;
    uint64_t       next_rid;
    uint64_t       format_fail;
    uint64_t       drop_oldest;
};

edge_ws_hub_t *edge_ws_hub_create(size_t max_subs)
{
    edge_ws_hub_t *h;

    if (max_subs == 0) {
        max_subs = 64;
    }
    h = (edge_ws_hub_t *)calloc(1, sizeof(*h));
    if (!h) {
        return NULL;
    }
    h->subs = (edge_ws_sub_t *)calloc(max_subs, sizeof(edge_ws_sub_t));
    if (!h->subs) {
        free(h);
        return NULL;
    }
    h->max_subs = max_subs;
    h->next_rid = 1;
    return h;
}

void edge_ws_hub_destroy(edge_ws_hub_t *h)
{
    size_t i;
    int j;

    if (!h) {
        return;
    }
    for (i = 0; i < h->max_subs; i++) {
        if (h->subs[i].active) {
            for (j = 0; j < EDGE_WS_PENDING_MAX; j++) {
                free(h->subs[i].q[j]);
                h->subs[i].q[j] = NULL;
            }
        }
    }
    free(h->subs);
    free(h);
}

int edge_ws_hub_subscribe(edge_ws_hub_t *h, int conn_slot)
{
    if (!h || conn_slot < 0 || (size_t)conn_slot >= h->max_subs) {
        return -1;
    }
    h->subs[conn_slot].active = 1;
    return 0;
}

void edge_ws_hub_unsubscribe(edge_ws_hub_t *h, int conn_slot)
{
    edge_ws_sub_t *s;
    int j;

    if (!h || conn_slot < 0 || (size_t)conn_slot >= h->max_subs) {
        return;
    }
    s = &h->subs[conn_slot];
    for (j = 0; j < EDGE_WS_PENDING_MAX; j++) {
        free(s->q[j]);
        s->q[j] = NULL;
        s->qlen[j] = 0;
    }
    s->head = s->tail = s->n = 0;
    s->active = 0;
}

int edge_ws_hub_is_subscribed(const edge_ws_hub_t *h, int conn_slot)
{
    if (!h || conn_slot < 0 || (size_t)conn_slot >= h->max_subs) {
        return 0;
    }
    return h->subs[conn_slot].active;
}

void edge_ws_hub_mint_request_id(edge_ws_hub_t *h, const char *prefer,
                                 char *out, size_t out_sz)
{
    if (!out || out_sz == 0) {
        return;
    }
    if (prefer && prefer[0]) {
        snprintf(out, out_sz, "%s", prefer);
        return;
    }
    if (h) {
        snprintf(out, out_sz, "eh%016llx",
                 (unsigned long long)h->next_rid++);
    } else {
        snprintf(out, out_sz, "eh0");
    }
}

static int sub_push(edge_ws_sub_t *s, const char *msg, size_t len,
                    edge_ws_hub_t *h)
{
    char *copy;

    if (!s->active) {
        return -1;
    }
    if (s->n >= EDGE_WS_PENDING_MAX) {
        /* drop oldest */
        free(s->q[s->head]);
        s->q[s->head] = NULL;
        s->qlen[s->head] = 0;
        s->head = (s->head + 1) % EDGE_WS_PENDING_MAX;
        s->n--;
        if (h) {
            h->drop_oldest++;
        }
    }
    copy = (char *)malloc(len + 1);
    if (!copy) {
        return -1;
    }
    memcpy(copy, msg, len);
    copy[len] = '\0';
    s->q[s->tail] = copy;
    s->qlen[s->tail] = len;
    s->tail = (s->tail + 1) % EDGE_WS_PENDING_MAX;
    s->n++;
    return 0;
}

int edge_ws_hub_broadcast_state_changed(edge_ws_hub_t *h, const char *ns,
                                        const char *key, const char *op,
                                        const char *value, size_t value_len,
                                        const char *request_id)
{
    char msg[EDGE_WS_MSG_MAX];
    int mlen;
    size_t i;
    int n = 0;
    static const char trunc_json[] = "{\"truncated\":true}";

    if (!h || !ns || !key || !op) {
        return 0;
    }
    mlen = edge_ws_format_state_changed(msg, sizeof(msg), ns, key, op, value,
                                        value_len, request_id);
    if (mlen < 0) {
        h->format_fail++;
        /* Compact fallback so SPA can refetch full value. */
        mlen = edge_ws_format_state_changed(msg, sizeof(msg), ns, key, op,
                                            trunc_json, sizeof(trunc_json) - 1,
                                            request_id);
        if (mlen < 0) {
            mlen = edge_ws_format_state_changed(msg, sizeof(msg), ns, key, op,
                                                NULL, 0, request_id);
            if (mlen < 0) {
                return 0;
            }
        }
    }
    for (i = 0; i < h->max_subs; i++) {
        if (h->subs[i].active) {
            if (sub_push(&h->subs[i], msg, (size_t)mlen, h) == 0) {
                n++;
            }
        }
    }
    return n;
}

int edge_ws_hub_take_pending(edge_ws_hub_t *h, int conn_slot, char *out,
                             size_t out_cap, size_t *out_len)
{
    edge_ws_sub_t *s;

    if (!h || !out || out_cap == 0 || conn_slot < 0 ||
        (size_t)conn_slot >= h->max_subs) {
        return -1;
    }
    s = &h->subs[conn_slot];
    if (!s->active || s->n == 0) {
        return 0;
    }
    if (s->qlen[s->head] + 1 > out_cap) {
        return -1;
    }
    memcpy(out, s->q[s->head], s->qlen[s->head] + 1);
    if (out_len) {
        *out_len = s->qlen[s->head];
    }
    free(s->q[s->head]);
    s->q[s->head] = NULL;
    s->qlen[s->head] = 0;
    s->head = (s->head + 1) % EDGE_WS_PENDING_MAX;
    s->n--;
    return 1;
}

uint64_t edge_ws_hub_format_fail_count(const edge_ws_hub_t *h)
{
    return h ? h->format_fail : 0;
}

uint64_t edge_ws_hub_drop_oldest_count(const edge_ws_hub_t *h)
{
    return h ? h->drop_oldest : 0;
}

size_t edge_ws_hub_subscriber_count(const edge_ws_hub_t *h)
{
    size_t i, n = 0;
    if (!h) {
        return 0;
    }
    for (i = 0; i < h->max_subs; i++) {
        if (h->subs[i].active) {
            n++;
        }
    }
    return n;
}
